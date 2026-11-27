// 介面的狀態與收發流程，照 macOS 版的 AppModel.swift 與 RadioEngine.swift。這個物件只在主執行緒上使用；
// 花時間的事（解調、影像編碼、PTT、縮圖、開檔）丟到各自的 Worker，做完再用 post_main 回主執行緒。
// 主迴圈每一圈呼叫一次 tick()：畫面的更新、發射的排程、定時發射都在裡面照時間做。
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio.hpp"
#include "canvas.hpp"
#include "core_bridge.hpp"
#include "ptt.hpp"
#include "settings.hpp"
#include "sources.hpp"
#include "tasks.hpp"
#include "waterfall.hpp"

namespace nbtvgui {

/// 接收端的一次更新，在主執行緒上交給介面。
struct RxUpdate {
    /// 第幾次開始接收的結果。重新開始接收（換裝置、發射結束）之後，先前那一次晚到的更新要丟掉。
    int generation = 0;
    /// 這一份有沒有接收機的狀態（下面六項）與解出來的幀；只帶頻譜列的更新沒有。
    bool has_status = false;
    bool synced = false;
    int mode_id = -1;
    bool snr_valid = false;
    double snr_db = 0;
    double offset_hz = 0;
    double input_level_db = -120;
    std::vector<DecodedFrame> frames;
    std::vector<std::array<float, kSpectrumBins>> spectrum_rows;
    /// 這幾列頻譜進來的時候接收機是不是同步中（同步中瀑布圖的參考位準不跟著訊號動）。
    bool rows_synced = false;
    /// 「同時送語音」開著時：語音那一路目前有沒有同步、訊雜比的估計。沒開著時是空的
    std::optional<bool> voice_synced;
    double voice_snr_db = 0;
};

class App {
public:
    enum class LastResult { none, ok, failed, model_missing, skipped };
    enum class TxPhase { idle, starting, transmitting, stopping };

    /// 收到的一張畫面。
    struct ReceivedFrame {
        std::uint64_t id = 0;
        PicturePtr picture;
        int mode_id = 0;
        std::string callsign;
        int sequence = -1;    // 幀序號；不知道時是 -1
        double time = 0;      // 收到的時刻（1970 年起的秒數）
        double snr_db = 0;    // 收到這一張時接收機量到的訊雜比（3 kHz 雜訊頻寬）
    };

    /// live 是 false 時不碰任何裝置（算示範圖用）。
    App(Settings initial, bool live);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    Settings settings;
    const bool live;

    // ---- 接收
    bool synced = false;
    std::string sync_callsign;
    std::optional<double> snr_db;
    std::optional<double> offset_hz;
    std::optional<int> rx_mode_id;
    LastResult last_result = LastResult::none;
    /// 這次執行以來，因為這台電腦來不及解而略過的畫面數（幀有收到，只是沒有解成畫面）。
    int skipped_frames = 0;
    double input_level_db = -120;
    std::deque<ReceivedFrame> history;      // 新的在前
    std::optional<std::uint64_t> selected_id;
    WaterfallStore waterfall;
    bool exporting_video = false;

    // ---- 發射
    TxPhase tx_phase = TxPhase::idle;
    int tx_mode_id = 11;
    double tx_elapsed = 0;
    int tx_frames_sent = 0;
    int tx_frame_number = 0;
    double tx_frame_progress = 0;
    PicturePtr tx_preview;

    // ---- 發射來源
    PicturePtr source_preview;
    std::optional<std::pair<int, int>> source_size;
    std::optional<std::string> source_message;
    std::vector<CameraInfo> cameras;
    std::optional<std::string> video_name;
    bool video_is_image = false;
    double video_duration = 0;
    double video_position = 0;
    bool video_playing = false;
    /// 圖片序列：要送的圖片檔（路徑），照發射的順序。空的就是還沒選。要換一批請用 open_images
    std::vector<std::string> image_files;
    /// 圖片序列：下一張要送的是第幾張（0 起算）。右側欄的預覽就是這一張。要改請用 select_image
    int image_index = 0;

    // ---- 定時發射
    bool schedule_armed = false;                       // 不存進設定：每次開 app 都是關的
    std::optional<double> schedule_next_fire;
    std::optional<int> schedule_countdown;            // 只在最後一分鐘裡有值
    std::optional<std::string> schedule_last_event;
    bool scheduled_run = false;
    bool show_schedule_menu = false;

    // ---- 裝置
    std::vector<AudioDevice> input_devices;
    std::vector<AudioDevice> output_devices;
    std::vector<std::string> serial_ports;
    std::string input_device_name = "—";
    std::string output_device_name = "—";
    // ---- 同時送語音（見 voice.hpp）
    std::string microphone_name = "—";
    std::string speaker_name = "—";
    bool voice_rx_synced = false;                // 收到的訊號裡有語音，而且同步到了
    std::optional<double> voice_rx_snr_db;       // 那時候語音的訊雜比
    bool voice_tx_on = false;                    // 這一次發射正在送語音（麥克風那一條路開起來了）
    std::optional<double> voice_tx_level_db;     // 那時候麥克風的音量（dBFS）
    /// 「同時送語音」現在有沒有作用：程式裡有語音的編解碼器，而且開關開著。
    bool voice_on() const;
    /// 現在按下發射的話會不會和語音一起送：開關開著，而且選的是 FM 用的模式。
    /// 定時發射一律不送語音：沒有人在的時候不開麥克風。
    bool voice_with_transmit() const;
    /// 設定裡「同時送語音」的開關、版面、麥克風或喇叭改了：存起來，接收照新的設定重來（發射中不動）。
    void voice_settings_changed();

    /// 要讓使用者看到的訊息（裝置或 PTT 出錯等），顯示在底列。
    std::optional<std::string> notice;
    /// PTT 的指令沒有成功、電台可能還按著發射時的警告（主視窗疊一條顯示，要使用者自己按掉）。
    std::optional<std::string> ptt_warning;
    bool show_mode_menu = false;

    /// 開發用：重要的流程轉折會經由它印出來。
    static std::function<void(const std::string&)> trace;
    /// 現在幾點（1970 年起的秒數）。測試時換成假的時鐘，不必真的等到整點。
    std::function<double()> clock;

    // 流程裡的幾個時間；測試時改小
    static double output_stall_seconds;      // 輸出裝置超過這麼久沒有來取取樣，就當成它壞了：立刻停止發射、放開 PTT
    static double schedule_margin_seconds;   // 定時發射最多送「影片的長度＋兩張＋這麼多秒」
    static double still_schedule_seconds;    // 定時發射的來源是圖片時送多久
    static constexpr double kPttLeadSeconds = 0.3;      // 按下 PTT 後等多久才開始送音訊
    static constexpr double kAbortGuardSeconds = 0.5;   // 「結束發射中」要過這麼久，再按一次才算「立刻停止」
    static constexpr double kPrepareLeadSeconds = 0.6;  // 目前這張剩多久時開始準備下一張（至少）
    static constexpr double kScheduleLateLimit = 5.0;   // 到了時間卻晚了這麼久才輪到：那一次不發射
    static constexpr double kSequenceGapSeconds = 10.0; // 兩張畫面之間的空檔要小於這麼多秒，才算同一段
    static constexpr std::size_t kHistoryLimit = 5000;  // 「最近收到」最多留幾張
    /// 最後一張後面補多長的靜音才放開 PTT：音效卡手上還有一小段沒播完的聲音，等它出去
    static constexpr double kTailSilenceSeconds = 0.25;

    // ---- 啟動與結束
    /// 開始接收、打開發射來源。app 啟動後呼叫一次。
    void start();
    /// app 結束前呼叫：停止輸出並放開 PTT（同步做完才回來）。
    void shutdown();
    /// 主迴圈每一圈呼叫。
    void tick();

    // ---- 裝置
    void refresh_devices();
    /// 設定裡的輸入或輸出裝置改了
    void devices_changed();

    // ---- 接收
    void apply(const RxUpdate& update);
    void add(const DecodedFrame& frame, double time);
    const ReceivedFrame* displayed_frame() const;
    void select(const ReceivedFrame& frame);
    bool is_displayed(const ReceivedFrame& frame) const;
    /// 一張收到的畫面存檔時預設的檔名
    static std::string export_file_name(const ReceivedFrame& frame);
    /// 把主畫面顯示的那一張存成 PNG：和收到的畫面一樣大（96×72，模式 4–7 與 15 是 192×144），像素完全相同。
    void save_displayed_frame(const std::string& path);
    /// 那一張畫面所在的「一段」：同一個呼號、一張接一張，中間的空檔都不到 10 秒。回傳的順序由舊到新。
    static std::vector<ReceivedFrame> sequence(std::uint64_t id, const std::deque<ReceivedFrame>& history);
    std::vector<ReceivedFrame> displayed_sequence() const;
    static std::string video_file_name(const std::vector<ReceivedFrame>& frames);
    /// 影片裡的一張畫面：從影片的第幾秒開始顯示，顯示到下一張開始為止。label 不是空的話，印在這一張的左上角（「SNR 12.3 dB」）。
    struct VideoPicture {
        PicturePtr picture;
        double start = 0;
        std::string label;
    };
    struct VideoTimeline {
        std::vector<VideoPicture> pictures;
        double last_hold = 0;   // 最後一張停留幾秒
    };
    /// 影片的時間軸：每一張從它收到的時刻開始顯示，到下一張收到為止，所以影片的快慢和當時收到的一樣；
    /// 最後一張停留它那個模式一張的長度（至少一秒）。
    /// stamp_snr：每一張的左上角印上收到它當下的訊雜比（寫法和主畫面上疊的標示一樣：「SNR 12.3 dB」）。
    static VideoTimeline video_timeline(const std::vector<ReceivedFrame>& frames, bool stamp_snr = false);
    /// 把一段畫面存成影片檔（.mp4，靠 ffmpeg）。completion 在主執行緒上呼叫（測試用）。
    /// 要不要在每一張的左上角印上訊雜比，照設定（settings.stamp_video_snr，按「儲存影片」時的那個選項）。
    void export_sequence(const std::vector<ReceivedFrame>& frames, const std::string& path, std::function<void(bool)> completion = {});

    // ---- 發射來源
    void set_source_kind(SourceKind kind);
    void select_camera(const std::string& name);
    std::string camera_name() const;
    void restart_source();
    /// 開啟一個影片檔或圖片。remember：記下來，下次啟動時自動開啟。
    void open_video(const std::string& path, bool remember, std::optional<bool> autoplay = std::nullopt, std::function<void(bool)> completion = {});
    void toggle_video_playback();
    void scrub_video(double seconds, bool editing);
    /// 圖片序列：換一批圖片。選到的資料夾換成它裡面的圖片，照檔名排好，從第一張開始。回傳有幾張；
    /// 一張圖片都沒有的話維持原來那一批，在底列說明。remember：記下來，下次啟動時自動帶回來。
    int open_images(const std::vector<std::string>& selection, bool remember);
    /// 圖片序列：改「下一張要送的」是第幾張（0 起算；超出範圍就停在第一張或最後一張）。
    void select_image(int index);
    /// 下一張要送的那個圖片檔；還沒選圖片時是空的。
    std::optional<std::string> next_image() const;
    /// 拖進視窗的檔案：一個影片檔（或一張圖片）當成在「影片檔」那裡選了它；好幾張圖片、資料夾，
    /// 或來源已經是圖片序列時拖進來的圖片，當成選了一批圖片序列。發射來源也換過去。
    void open_dropped(const std::vector<std::string>& paths);
    void set_canvas_enabled(bool on);
    /// 送出的畫面多大：跟著發射模式（模式 3 與 11–14 是 96×72，模式 4–7 與 15 是 192×144）。
    /// 沒在發射時是設定裡選的模式，發射中是這一次發射用的模式。
    PictureSize tx_picture_size() const;
    /// 把畫布的字印到一張來源畫面上（畫布沒開就原樣回傳）。發射前的每一張與右側欄的預覽都經過這裡。
    /// 畫面要是送出的大小（tx_picture_size）；大小不對的原樣回傳。
    Picture apply_canvas(const Picture& picture);
    /// 有來源畫面可以發射嗎？
    bool can_transmit() const;

    // ---- 發射
    void toggle_transmit();
    /// from_start：來源是影片檔時先回到開頭再開始（定時發射用）。
    void start_transmit(bool from_start = false);
    void stop_transmit();
    void abort_transmit();
    /// 測試用：之後的發射不開音訊裝置，改由模擬的輸出照實際的時間把取樣取走。
    void debug_use_simulated_output(std::function<void(const std::vector<float>&)> sink, std::optional<double> stall_after = std::nullopt);
    /// 測試用：正在和語音一起送時，當成麥克風收到了這一段語音（48 kHz）。
    void debug_speak(std::vector<float> samples);
    /// 測試用：喇叭換成模擬的（解出來的語音交給 sink，在別的執行緒上呼叫）。要在 debug_begin_receiving 之前呼叫。
    void debug_use_simulated_speaker(std::function<void(const std::vector<float>&)> sink);
    /// 測試用：不開音訊裝置就開始（或結束）一次接收；音訊由 debug_feed_input 一段一段交進來，像音訊的執行緒那樣。
    void debug_begin_receiving();
    void debug_stop_receiving();
    void debug_feed_input(std::vector<float> samples);
    /// 測試用：開著的時候，接收的那條執行緒每解出一張畫面就停住（debug_decoder_held 變成 true），到關掉為止——
    /// 等於一台解一張畫面要很久的電腦（停止或重新開始接收也打斷不了它，和真的解碼一樣）。
    void debug_hold_decoder(bool hold);
    bool debug_decoder_held() const;
    /// 測試用：交進來、接收機還沒處理的音訊有幾秒。
    double debug_receive_backlog() const;

    // ---- 示範圖與測試用
    /// 當成來源已經有畫面：live 是 false 的示範圖裡，發射鈕才是可以按的樣子。
    bool demo_source_ready = false;
    /// 影片目前那一格是第幾格（每換一格加一）；還沒有任何一格時是 0。
    std::uint64_t demo_video_frame_version() const;
    /// 把影片目前那一格縮成預覽（live 是 false 時 ui_tick 不跑，示範圖要自己取）。還沒有任何一格時回傳 false。
    bool demo_grab_video_frame();
    /// 把「下一張要送的」那一張圖片直接讀進來當預覽（同樣是給示範圖用的）。讀到了回傳 true。
    bool demo_load_image();
    /// 測試用：把一張畫面直接當成來源目前的那一格（不經過鏡頭或檔案）。
    void debug_set_source_frame(SourceFramePtr frame);

    // ---- 定時發射
    void set_schedule_armed(bool on);
    void schedule_interval_changed();
    /// 定時發射到了時間也發射不了的原因；沒有問題時是空的。
    std::optional<std::string> schedule_problem() const;
    /// 頂列「定時發射」鈕上的字。
    std::string schedule_summary() const;

    // ---- 顯示用的文字
    const ModeInfo* tx_mode() const { return modes::find(tx_mode_id); }
    bool is_on_air() const { return tx_phase == TxPhase::transmitting || tx_phase == TxPhase::stopping; }
    /// 發射來源是只播一次的影片檔（沒有勾循環播放，或這一次是定時發射）
    bool video_plays_once() const;
    /// 發射來源是沒有勾循環播放的圖片序列：從預覽的那一張送到最後一張，送完自動停止發射。
    bool images_play_once() const;
    static std::string clock_text(double seconds);         // 「00:23」
    static double level_fraction(double db, double floor = -50);
    static std::string snr_text(double db);                // 「12.3 dB」
    static std::string db_text(double db);                 // 「−18 dB」

private:
    struct Engine;
    struct OnAirFrame {
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::vector<float> samples;
        PicturePtr preview;                 // 預覽另外解的模式（ModeInfo::preview_apart）排進來時還沒有，解好了才補上
        int number = 0;
        std::optional<std::string> image;   // 來源是圖片序列時，這一張是哪一個圖片檔
    };
    /// 預覽另外解的那一張，等著解（見 schedule_transmit_preview）。
    struct PendingPreview {
        std::vector<float> values;
        int mode_id = 0;
        int number = 0;
        int session = 0;
    };
    /// 取來源的一格的結果（見 capture_source_frame）。
    struct SourceCapture {
        std::optional<Picture> rgb;         // 縮成送出的大小的畫面，還沒有印上畫布的字；來源沒有畫面時是空的
        bool last = false;                  // 來源到此為止：只播一次的影片播完了（這是它的最後一格），或圖片序列已經沒有圖片可以送（這時 rgb 是空的）
        std::optional<std::string> image;   // 圖片序列：這一格是哪一個圖片檔
    };
    /// 圖片序列的預覽是照什麼算出來的：哪一張、哪一種縮放方式、哪一版的畫布、送出的畫面多大（跟著發射模式）。
    /// 和現在該有的不同就重算（見 refresh_image_preview）
    struct ImagePreviewKey {
        std::string path;
        ScaleMode scale = ScaleMode::fill;
        int canvas = 0;
        PictureSize size;
        bool operator==(const ImagePreviewKey& other) const { return path == other.path && scale == other.scale && canvas == other.canvas && size == other.size; }
    };
    struct TransmitDisplay {
        int number = 0;
        PicturePtr preview;
        double progress = 0;
        int sent = 0;
        double elapsed = 0;
    };

    void post_main(std::function<void()> job);
    /// 過這麼多秒之後在主執行緒上做（由 tick 照時間取出來做）。
    void after(double seconds, std::function<void()> job);
    void warn_ptt_may_be_keyed(const std::string& reason);
    void start_receiving();
    void start_speaker();
    void start_voice_path(VoiceLayout layout);
    void clear_sync_state();
    void clear_voice_state();
    void append_spectrum(const std::vector<std::array<float, kSpectrumBins>>& rows, WaterfallStore::Reference reference);

    std::shared_ptr<const CanvasLayer> current_canvas();
    void clear_source_frame();
    void start_camera();
    void reopen_last_video();
    void ui_tick();
    void publish_transmit_display();
    void refresh_preview();
    void capture_source_frame(std::function<void(SourceCapture)> completion);

    void set_images(std::vector<std::string> files);
    void reopen_last_images();
    void refresh_image_preview();
    void remove_unreadable_images(const std::vector<std::string>& unreadable);
    void capture_next_image(std::function<void(SourceCapture)> completion);
    bool accept(const SourceCapture& capture);
    void put_back_image(const std::optional<std::string>& path);

    void wait_for_video_frame(int session, std::uint64_t since, int attempts, std::function<void()> then);
    bool video_finished() const;
    void first_frame_ready(std::optional<EncodedFrame> frame, int session);
    void go_on_air(EncodedFrame first, int session);
    bool enqueue(const EncodedFrame& frame);
    void schedule_transmit_preview(std::vector<float> values, int mode_id, int number);
    void decode_next_transmit_preview();
    void tx_tick();
    void prepare_next_frame();
    void stop_after_queued_frames();
    void pause_video_with_transmit();
    std::optional<OnAirFrame> drop_queued_frame();
    void queue_tail();
    void finish_transmit();
    void fail_transmit(const std::optional<std::string>& message);
    void end_transmit(const std::optional<std::string>& failure);
    void release_ptt();

    void reschedule_next();
    void schedule_tick();
    void fire_schedule(const std::string& label);
    bool receiving_someone() const;
    void skip_schedule(const std::string& label, const std::string& reason);
    void finish_scheduled_run(const std::optional<std::string>& failure, std::optional<int> sent = std::nullopt);

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::unique_ptr<Engine> engine_;
    CameraSource camera_;
    FileSource video_;
    ImageSequence images_;   // 只在 preview_worker_ 上使用
    std::optional<ImagePreviewKey> image_preview_key_;
    bool image_preview_loading_ = false;
    std::uint64_t next_frame_id_ = 1;

    // 來源目前的那一格
    SourceFramePtr latest_frame_;
    std::uint64_t latest_version_ = 0;
    std::uint64_t video_frame_version_ = 0;
    std::uint64_t preview_version_ = static_cast<std::uint64_t>(-1);
    std::optional<ScaleMode> preview_scale_;
    std::optional<PictureSize> preview_picture_;   // 預覽是照多大的畫面算的：換到畫面大小不同的模式時要重算
    // 畫布：圖層照設定與畫面的大小算好留著，改了才重算
    std::shared_ptr<const CanvasLayer> canvas_layer_;
    std::optional<CanvasStyle> canvas_built_;
    PictureSize canvas_built_picture_;
    bool canvas_ever_built_ = false;
    int canvas_version_ = 0;
    int preview_canvas_version_ = 0;
    double preview_time_ = -1e9;
    int source_epoch_ = 0;   // 每換一次來源加一；換來源之前開始算的預覽晚到時靠它認出來
    std::optional<Picture> last_source_rgb_;
    bool scrubbing_ = false;
    bool opening_video_ = false;
    double last_seek_time_ = -1e9;
    std::vector<std::pair<double, std::function<void()>>> timers_;   // 延後的工作：到時間的時刻與要做的事

    std::vector<std::array<float, kSpectrumBins>> queued_monitor_rows_;
    std::optional<TransmitDisplay> tx_display_;
    double last_ui_tick_ = -1e9;
    double last_tx_tick_ = -1e9;
    double last_device_refresh_ = -1e9;
    bool started_ = false;
    bool receive_wanted_ = false;
    std::optional<std::string> active_input_;   // 目前收音的裝置的名稱；沒有開成功時是空的

    // 發射流程
    std::shared_ptr<PTTDriver> ptt_driver_;
    std::deque<OnAirFrame> on_air_;
    int tx_session_ = 0;            // 每次開始或結束發射就加一；晚到的非同步結果靠它認出自己已經過期
    std::int64_t tx_lead_ = 0;      // 開頭的靜音（等電台切到發射）
    std::int64_t tx_end_ = 0;       // 已排進輸出佇列的最後位置
    std::int64_t tx_monitored_ = 0; // 已交給頻譜分析的位置
    int tx_next_number_ = 1;
    bool tx_preparing_ = false;
    // 預覽另外解（ModeInfo::preview_apart）：一次只解一張。上一張還沒解完又排進新的一張，等著的那一張就換成新的
    // （電腦慢的時候預覽跳著更新，發射不受影響）。
    bool tx_preview_busy_ = false;
    std::optional<PendingPreview> tx_preview_waiting_;
    int tx_tail_state_ = 0;         // 0：還沒排尾巴，1：正在取，2：已排入
    std::optional<Picture> tx_last_rgb_;   // 這次發射最近送出的那一格；來源一時沒有畫面時重送它
    std::optional<std::string> tx_pending_image_;   // 圖片序列：已經取走、正在編碼，還沒排進輸出佇列的那一張（見 put_back_image）
    bool tx_ends_after_first_ = false;              // 圖片序列只剩一張要送：第一張排進去之後就收尾（見 go_on_air）
    std::int64_t tx_pulled_frames_ = -1;
    double tx_pulled_at_ = 0;
    double tx_stopping_at_ = 0;
    double tx_prepare_seconds_ = 0;        // 最近準備一張（取畫面＋編碼）花了多久
    std::optional<VoiceLayout> tx_voice_layout_;   // 這一次發射是和語音一起送的話，用的是哪一種版面
    bool tx_voice_active_ = false;         // 語音那一條路真的開起來了（麥克風開不了的話畫面照送，只是沒有語音）
    bool tx_voice_finished_ = false;       // 這一次發射的語音已經收尾了
    double prepare_lead() const;

    // 定時發射
    std::string schedule_label_;
    std::optional<SourceKind> schedule_restore_source_;
    std::optional<double> schedule_limit_;
    bool schedule_limit_is_normal_ = false;   // 到了上限是正常結束（來源是圖片），不算「超過影片的長度」
    bool schedule_cut_short_ = false;
    bool sleep_inhibited_ = false;
    double last_signal_time_ = -1e18;

    // 各自一條執行緒；宣告在最後，解構時最先結束（它們的工作會用到上面的成員）
    std::unique_ptr<Worker> tx_worker_;
    std::unique_ptr<Worker> tx_preview_worker_;   // 預覽另外解的模式（現在是模式 4）：「對方看到的畫面」在這裡解，不擋住編碼
    std::unique_ptr<Worker> ptt_worker_;
    std::unique_ptr<Worker> preview_worker_;
    std::unique_ptr<Worker> io_worker_;
};

}  // namespace nbtvgui
