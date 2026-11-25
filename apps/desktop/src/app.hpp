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
    bool synced = false;
    int mode_id = -1;
    bool snr_valid = false;
    double snr_db = 0;
    double offset_hz = 0;
    double input_level_db = -120;
    std::vector<DecodedFrame> frames;
    std::vector<std::array<float, kSpectrumBins>> spectrum_rows;
};

class App {
public:
    enum class LastResult { none, ok, failed, model_missing };
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
    /// 把主畫面顯示的那一張存成 96×72 的 PNG。
    void save_displayed_frame(const std::string& path);
    /// 那一張畫面所在的「一段」：同一個呼號、一張接一張，中間的空檔都不到 10 秒。回傳的順序由舊到新。
    static std::vector<ReceivedFrame> sequence(std::uint64_t id, const std::deque<ReceivedFrame>& history);
    std::vector<ReceivedFrame> displayed_sequence() const;
    static std::string video_file_name(const std::vector<ReceivedFrame>& frames);
    /// 把主畫面顯示的那一張所在的一段存成影片檔（.mp4，靠 ffmpeg）。completion 在主執行緒上呼叫（測試用）。
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
    void set_canvas_enabled(bool on);
    /// 把畫布的字印到一張來源畫面上（畫布沒開就原樣回傳）。
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

    // ---- 示範圖與測試用
    /// 當成來源已經有畫面：live 是 false 的示範圖裡，發射鈕才是可以按的樣子。
    bool demo_source_ready = false;
    /// 影片目前那一格是第幾格（每換一格加一）；還沒有任何一格時是 0。
    std::uint64_t demo_video_frame_version() const;
    /// 把影片目前那一格縮成預覽（live 是 false 時 ui_tick 不跑，示範圖要自己取）。還沒有任何一格時回傳 false。
    bool demo_grab_video_frame();
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
        PicturePtr preview;
        int number = 0;
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
    void clear_sync_state();
    void append_spectrum(const std::vector<std::array<float, kSpectrumBins>>& rows, WaterfallStore::Reference reference);

    std::shared_ptr<const CanvasLayer> current_canvas();
    void clear_source_frame();
    void start_camera();
    void reopen_last_video();
    void ui_tick();
    void publish_transmit_display();
    void refresh_preview();
    void capture_source_frame(std::function<void(std::optional<Picture>)> completion);

    void wait_for_video_frame(int session, std::uint64_t since, int attempts, std::function<void()> then);
    bool video_finished() const;
    void first_frame_ready(std::optional<EncodedFrame> frame, int session);
    void go_on_air(EncodedFrame first, int session);
    bool enqueue(const EncodedFrame& frame);
    void tx_tick();
    void prepare_next_frame();
    void stop_after_queued_frames();
    void pause_video_with_transmit();
    void drop_queued_frame();
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
    std::uint64_t next_frame_id_ = 1;

    // 來源目前的那一格
    SourceFramePtr latest_frame_;
    std::uint64_t latest_version_ = 0;
    std::uint64_t video_frame_version_ = 0;
    std::uint64_t preview_version_ = static_cast<std::uint64_t>(-1);
    std::optional<ScaleMode> preview_scale_;
    // 畫布：圖層照設定算好留著，設定改了才重算
    std::shared_ptr<const CanvasLayer> canvas_layer_;
    std::optional<CanvasStyle> canvas_built_;
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
    int tx_tail_state_ = 0;         // 0：還沒排尾巴，1：正在取，2：已排入
    std::optional<Picture> tx_last_rgb_;   // 這次發射最近送出的那一格；來源一時沒有畫面時重送它
    std::int64_t tx_pulled_frames_ = -1;
    double tx_pulled_at_ = 0;
    double tx_stopping_at_ = 0;
    double tx_prepare_seconds_ = 0;        // 最近準備一張（取畫面＋編碼）花了多久
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
    std::unique_ptr<Worker> ptt_worker_;
    std::unique_ptr<Worker> preview_worker_;
    std::unique_ptr<Worker> io_worker_;
};

}  // namespace nbtvgui
