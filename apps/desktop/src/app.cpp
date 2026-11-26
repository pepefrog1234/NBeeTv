#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "i18n.hpp"
#include "png.hpp"
#include "process.hpp"

namespace nbtvgui {

std::function<void(const std::string&)> App::trace;
double App::output_stall_seconds = 3.0;
double App::schedule_margin_seconds = 5.0;
double App::still_schedule_seconds = 30.0;

namespace {

void note(const std::string& text) {
    if (App::trace) App::trace(text);
}

std::string base_name(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string trim(const std::string& text) {
    std::size_t first = 0, last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\n' || text[first] == '\r')) ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\n' || text[last - 1] == '\r')) --last;
    return text.substr(first, last - first);
}

std::string slashes_to_dashes(std::string text) {
    std::replace(text.begin(), text.end(), '/', '-');
    return text;
}

}  // namespace

// ---------------------------------------------------------------- 音訊裝置與核心之間的橋樑（macOS 版的 RadioEngine）
//
// 核心的接收機與頻譜分析都只在 core 這條執行緒上使用，不在音訊的執行緒、也不在主執行緒上做解調
// （一幀結束時 nbtv_rx_push 可能花幾十毫秒，慢的電腦更久）。發射機只在 App 的 tx_worker_ 上使用。

struct App::Engine {
    /// 輸出佇列要放得下開頭的靜音、一整張，再加上提早排進去的下一張；照最長的模式算，至少 14 秒。
    static double queue_seconds() {
        double longest = 6;
        for (const ModeInfo& mode : modes::all()) longest = std::max(longest, mode.frame_seconds);
        return std::max(14.0, 2 * longest + 2);
    }

    AudioOutput output{queue_seconds()};
    AudioInput input;
    CoreReceiver receiver;
    CoreSpectrum spectrum;
    CoreTransmitter transmitter;
    /// 開始接收了幾次（主執行緒上讀寫）。
    int generation = 0;
    /// 接收的更新、發射監看的頻譜列：在 core 的執行緒上呼叫，由 App 轉回主執行緒。
    std::function<void(RxUpdate)> on_rx_update;
    std::function<void(std::vector<std::array<float, kSpectrumBins>>)> on_monitor_rows;

    // 以下只在 core 的執行緒上讀寫
    bool receiving = false;
    int active_generation = 0;
    RxUpdate pending;          // 還沒交給介面的更新
    double last_delivery = 0;
    /// 音訊每十幾毫秒進來一段，但介面不需要更新得那麼勤：攢到這麼久才交一次；解出新的一幀時立刻交。
    static constexpr double kDeliveryInterval = 0.08;

    Worker core;   // 宣告在最後：解構時最先結束

    Engine() {
        // app 接收用的接收機一律把最近幾幀合起來解（畫面沒變的地方合併前面幾張，訊號弱時比較清楚也比較穩定）
        receiver.set_merging(true);
        input.on_samples = [this](std::vector<float> samples) {
            core.post([this, samples = std::move(samples)] { process(samples); });
        };
    }

    ~Engine() {
        input.stop();
        output.stop();
    }

    bool is_receiving() const { return input.running(); }

    /// 從指定的輸入裝置開始接收；接收機會先清成初始狀態。
    bool start_receiving(SDL_AudioDeviceID device, std::string* error) {
        const int current = ++generation;
        core.sync([this, current] {
            receiver.reset();
            spectrum.reset();
            pending = RxUpdate();
            active_generation = current;
            receiving = true;
        });
        if (input.start(device, error)) return true;
        core.sync([this] { receiving = false; });
        return false;
    }

    void stop_receiving() {
        input.stop();
        core.sync([this] { receiving = false; });
    }

    /// 發射時把送出的音訊交給頻譜分析，瀑布圖顯示送出的訊號。
    void monitor(std::vector<float> samples) {
        core.post([this, samples = std::move(samples)] {
            spectrum.push(samples.data(), static_cast<int>(samples.size()));
            std::vector<std::array<float, kSpectrumBins>> rows;
            std::array<float, kSpectrumBins> row{};
            while (spectrum.poll(row)) rows.push_back(row);
            if (!rows.empty() && on_monitor_rows) on_monitor_rows(std::move(rows));
        });
    }

    void reset_spectrum() {
        core.sync([this] { spectrum.reset(); });
    }

    void process(const std::vector<float>& samples) {
        if (!receiving) return;
        receiver.push(samples.data(), static_cast<int>(samples.size()));
        spectrum.push(samples.data(), static_cast<int>(samples.size()));
        const nbtv_rx_status status = receiver.status();
        pending.synced = status.synced != 0;
        pending.mode_id = status.mode_id;
        pending.snr_valid = status.snr_valid != 0;
        pending.snr_db = status.snr_db;
        pending.offset_hz = status.freq_offset_hz;
        pending.input_level_db = status.input_level_db;
        while (std::optional<DecodedFrame> frame = receiver.poll_frame()) pending.frames.push_back(std::move(*frame));
        std::array<float, kSpectrumBins> row{};
        while (spectrum.poll(row)) pending.spectrum_rows.push_back(row);
        const double now = uptime_seconds();
        if (pending.frames.empty() && now - last_delivery < kDeliveryInterval) return;
        last_delivery = now;
        RxUpdate update = std::move(pending);
        update.generation = active_generation;
        pending = RxUpdate();
        if (on_rx_update) on_rx_update(std::move(update));
    }
};

// ---------------------------------------------------------------- 啟動與結束

App::App(Settings initial, bool live_) : settings(std::move(initial)), live(live_), clock(schedule::now) {
    tx_mode_id = settings.tx_mode_id;
    tx_worker_ = std::make_unique<Worker>();
    ptt_worker_ = std::make_unique<Worker>();
    preview_worker_ = std::make_unique<Worker>();
    io_worker_ = std::make_unique<Worker>();
    if (!live) return;
    engine_ = std::make_unique<Engine>();
    engine_->on_rx_update = [this](RxUpdate update) {
        post_main([this, update = std::move(update)] {
            // 重新開始接收之前留下來的不算
            if (update.generation == engine_->generation) apply(update);
        });
    };
    engine_->on_monitor_rows = [this](std::vector<std::array<float, kSpectrumBins>> rows) {
        post_main([this, rows = std::move(rows)] { queued_monitor_rows_.insert(queued_monitor_rows_.end(), rows.begin(), rows.end()); });
    };
    engine_->output.gain = settings.output_gain();
}

App::~App() {
    *alive_ = false;
    // 先讓各條執行緒把手上的工作做完再結束：它們的工作會用到下面還沒解構的成員
    io_worker_.reset();
    preview_worker_.reset();
    tx_worker_.reset();
    ptt_worker_.reset();
    engine_.reset();
}

void App::post_main(std::function<void()> job) {
    MainQueue::shared().post([alive = alive_, job = std::move(job)] {
        if (*alive) job();
    });
}

void App::after(double seconds, std::function<void()> job) { timers_.emplace_back(uptime_seconds() + seconds, std::move(job)); }

void App::start() {
    if (!live) return;
    refresh_devices();
    reopen_last_images();   // 先帶回來：下面打開來源時，圖片序列才有東西
    if (std::getenv("NBTV_NO_DEVICES") == nullptr) {
        receive_wanted_ = true;
        start_receiving();
        restart_source();
    }
    reopen_last_video();
    started_ = true;
}

void App::shutdown() {
    ++tx_session_;
    if (engine_) engine_->output.stop();
    if (ptt_driver_) {
        const std::shared_ptr<PTTDriver> driver = ptt_driver_;
        ptt_driver_.reset();
        ptt_worker_->sync([driver] { driver->key(false); });
    }
    if (engine_) engine_->stop_receiving();
    io_worker_->sync([] {});   // 正在開的檔案等它開完
    camera_.stop();
    video_.close();
    if (sleep_inhibited_) {
        SDL_EnableScreenSaver();
        sleep_inhibited_ = false;
    }
    settings.save();
}

void App::tick() {
    const double now = uptime_seconds();
    // 到時間的延後工作（做的時候可能再排新的，所以先拿出來）
    if (!timers_.empty()) {
        std::vector<std::function<void()>> due;
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->first <= now) {
                due.push_back(std::move(it->second));
                it = timers_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& job : due) job();
    }
    if (!live) return;
    if (engine_) engine_->output.gain = settings.output_gain();
    if (is_on_air() && now - last_tx_tick_ >= 0.05) {
        last_tx_tick_ = now;
        tx_tick();
    }
    if (now - last_ui_tick_ >= 0.1) {
        last_ui_tick_ = now;
        ui_tick();
    }
    if (started_ && now - last_device_refresh_ >= 5.0) {
        last_device_refresh_ = now;
        refresh_devices();
    }
}

/// PTT 的指令沒有成功，電台可能還按著：記下來並在主視窗上顯示警告（底列的一行字太容易沒看到，而且下一次發射就被清掉）。
void App::warn_ptt_may_be_keyed(const std::string& reason) {
    note("警告：PTT 可能還按著（" + reason + "）");
    ptt_warning = LF("PTT 的指令沒有成功（%@），電台可能還在發射。請看一下電台；還在發射的話，直接用電台上的按鍵或開關停下來。", {reason});
}

// ---------------------------------------------------------------- 裝置

void App::refresh_devices() {
    last_device_refresh_ = uptime_seconds();
    input_devices = audio_inputs();
    output_devices = audio_outputs();
    serial_ports = nbtvgui::serial_ports();
    cameras = CameraSource::devices();
    const std::optional<AudioDevice> input = resolve_audio_device(settings.input_device, true);
    input_device_name = input ? input->name : L("沒有輸入裝置");
    const std::optional<AudioDevice> output = resolve_audio_device(settings.output_device, false);
    output_device_name = output ? output->name : L("沒有輸出裝置");

    // 該用的輸入裝置和正在用的不一樣了（改了設定、裝置被拔掉或插回來，或是先前沒開成功）：改從現在該用的那一個接收。
    if (!engine_ || !receive_wanted_ || tx_phase != TxPhase::idle) return;
    const std::optional<std::string> wanted = input ? std::optional<std::string>(input->name) : std::nullopt;
    if (wanted != active_input_ || (wanted && !engine_->is_receiving())) {
        note("輸入裝置改變，重新開始接收");
        engine_->stop_receiving();
        start_receiving();
    }
}

void App::devices_changed() {
    settings.save();
    refresh_devices();
}

/// 從設定的輸入裝置開始接收（接收機會清成初始狀態）。發射中不做事。
void App::start_receiving() {
    if (!engine_ || !receive_wanted_ || tx_phase != TxPhase::idle) return;
    active_input_.reset();
    clear_sync_state();
    const std::optional<AudioDevice> device = resolve_audio_device(settings.input_device, true);
    if (!device) {
        notice = L("找不到音訊輸入裝置");
        return;
    }
    std::string error;
    if (engine_->start_receiving(device->id, &error)) {
        active_input_ = device->name;
        input_device_name = device->name;
        note("開始接收：輸入「" + device->name + "」");
    } else {
        notice = LF("無法開啟輸入裝置「%@」：%@", {device->name, error});
    }
}

// ---------------------------------------------------------------- 接收

void App::clear_sync_state() {
    synced = false;
    sync_callsign.clear();
    snr_db.reset();
    offset_hz.reset();
    rx_mode_id.reset();
    last_result = LastResult::none;
}

void App::apply(const RxUpdate& update) {
    if (tx_phase != TxPhase::idle && live) return;
    if (update.synced || !update.frames.empty()) last_signal_time_ = clock();
    input_level_db = update.input_level_db;
    for (const DecodedFrame& frame : update.frames) add(frame, schedule::now());
    if (update.synced) {
        synced = true;
        rx_mode_id = update.mode_id;
        snr_db = update.snr_valid ? std::optional<double>(update.snr_db) : std::nullopt;
        offset_hz = update.offset_hz;
    } else if (synced || update.frames.empty()) {
        clear_sync_state();
    }
    append_spectrum(update.spectrum_rows, update.synced ? WaterfallStore::Reference::hold : WaterfallStore::Reference::noise);
}

void App::add(const DecodedFrame& frame, double time) {
    if (!frame.decoded || !frame.picture) {
        // 幀收到了，但對方用的是另一份權重（或沒有那個模式的權重檔）
        last_result = frame.model_missing ? LastResult::model_missing : LastResult::failed;
        return;
    }
    last_result = LastResult::ok;
    if (frame.header_ok && !frame.callsign.empty()) sync_callsign = frame.callsign;
    ReceivedFrame received;
    received.id = next_frame_id_++;
    received.picture = frame.picture;
    received.mode_id = frame.mode_id;
    received.callsign = frame.header_ok ? frame.callsign : std::string();
    received.sequence = frame.header_ok ? frame.sequence : -1;
    received.time = time;
    received.snr_db = frame.snr_db;
    history.push_front(std::move(received));
    while (history.size() > kHistoryLimit) history.pop_back();
    // 有新的畫面進來：主畫面一律換成它。先前點選來看的那一張還在「最近收到」裡，要看可以再點
    selected_id.reset();
}

void App::append_spectrum(const std::vector<std::array<float, kSpectrumBins>>& rows, WaterfallStore::Reference reference) {
    for (const auto& row : rows) waterfall.append(row, reference);
}

/// 主畫面顯示的那一張：使用者點選的，沒有點選就是最新的。點選只維持到下一張畫面進來為止（見 add）。
const App::ReceivedFrame* App::displayed_frame() const {
    if (selected_id) {
        for (const ReceivedFrame& frame : history) {
            if (frame.id == *selected_id) return &frame;
        }
    }
    return history.empty() ? nullptr : &history.front();
}

void App::select(const ReceivedFrame& frame) {
    // 點最新的那張等於回到「跟著最新的」
    if (!history.empty() && frame.id == history.front().id) selected_id.reset();
    else selected_id = frame.id;
}

bool App::is_displayed(const ReceivedFrame& frame) const {
    const ReceivedFrame* shown = displayed_frame();
    return shown != nullptr && shown->id == frame.id;
}

std::string App::export_file_name(const ReceivedFrame& frame) {
    const std::string call = frame.callsign.empty() ? "unknown" : slashes_to_dashes(frame.callsign);
    return "NBTV-" + call + "-" + schedule::stamp_text(frame.time) + ".png";
}

void App::save_displayed_frame(const std::string& path) {
    const ReceivedFrame* frame = displayed_frame();
    if (frame == nullptr || !frame->picture) return;
    // 96×72 的 PNG，像素和收到的畫面完全相同
    if (!write_png(path, frame->picture->rgb.data(), kImageWidth, kImageHeight)) notice = LF("存檔失敗：%@", {path});
}

// 那一張畫面所在的「一段」：同一個呼號、一張接一張，中間的空檔都不到 10 秒。回傳的順序由舊到新。
//
// 空檔是前一張收完到後一張開始送之間的時間（後一張收完的時刻減掉它那個模式一張的長度，再減前一張收完的時刻），
// 所以連續發射時是 0，中間漏掉一張時是一張的長度。
// 標頭沒有解出來、不知道呼號的畫面，時間接得上就跟著前面那一段（它是一段的開頭的話，跟著後面的）。
// 每一張畫面只屬於一段：不論從一段裡的哪一張問，得到的都是同一段。
std::vector<App::ReceivedFrame> App::sequence(std::uint64_t id, const std::deque<ReceivedFrame>& history) {
    std::size_t index = history.size();
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (history[i].id == id) index = i;
    }
    if (index == history.size()) return {};
    // older 收完之後到 newer 開始送，中間的空檔夠不夠短
    const auto contiguous = [](const ReceivedFrame& older, const ReceivedFrame& newer) {
        const ModeInfo* mode = modes::find(newer.mode_id);
        const double started = newer.time - (mode != nullptr ? mode->frame_seconds : 0.0);
        return started - older.time < kSequenceGapSeconds;
    };
    // 1. 時間上接在一起的那一串（history 是新的在前：往後是更早的，往前是更晚的）
    std::size_t oldest = index, newest = index;
    while (oldest + 1 < history.size() && contiguous(history[oldest + 1], history[oldest])) ++oldest;
    while (newest > 0 && contiguous(history[newest], history[newest - 1])) --newest;
    // 2. 這一串由舊到新照呼號分段：出現和這一段不一樣的呼號就是下一段的開頭
    std::vector<std::vector<ReceivedFrame>> groups;
    std::string callsign;
    for (std::size_t i = oldest + 1; i-- > newest;) {
        const ReceivedFrame& frame = history[i];
        if (groups.empty() || (!frame.callsign.empty() && !callsign.empty() && frame.callsign != callsign)) {
            groups.push_back({frame});
            callsign = frame.callsign;
        } else {
            groups.back().push_back(frame);
            if (callsign.empty()) callsign = frame.callsign;
        }
    }
    for (const auto& group : groups) {
        for (const ReceivedFrame& frame : group) {
            if (frame.id == id) return group;
        }
    }
    return {};
}

std::vector<App::ReceivedFrame> App::displayed_sequence() const {
    const ReceivedFrame* frame = displayed_frame();
    return frame != nullptr ? sequence(frame->id, history) : std::vector<ReceivedFrame>();
}

std::string App::video_file_name(const std::vector<ReceivedFrame>& frames) {
    std::string known;
    for (const ReceivedFrame& frame : frames) {
        if (!frame.callsign.empty()) {
            known = frame.callsign;
            break;
        }
    }
    const std::string call = known.empty() ? "unknown" : slashes_to_dashes(known);
    return "NBTV-" + call + "-" + schedule::stamp_text(frames.empty() ? schedule::now() : frames.front().time) + ".mp4";
}

App::VideoTimeline App::video_timeline(const std::vector<ReceivedFrame>& frames, bool stamp_snr) {
    VideoTimeline timeline;
    if (frames.empty()) return timeline;
    for (const ReceivedFrame& frame : frames) {
        timeline.pictures.push_back({frame.picture, frame.time - frames.front().time, stamp_snr ? "SNR " + snr_text(frame.snr_db) : std::string()});
    }
    const ModeInfo* last_mode = modes::find(frames.back().mode_id);
    timeline.last_hold = std::max(1.0, last_mode != nullptr ? last_mode->frame_seconds : 1.0);
    return timeline;
}

// 每秒 24 格、960×720（每個像素放大成 10×10，不內插）。編碼交給 ffmpeg：把 96×72 的畫面一格一格從管線送給它，由它放大。
//
// 要印訊雜比的時候，字是這裡自己畫的（見 VideoStamp）：字只蓋住影片左上角的一小塊，所以不把 960×720 的整格畫面送過去
// （一格 2 MB，管線會慢得多），而是把「印好字的那一小塊」接在 96×72 的畫面右邊一起送——送過去的一格是
// 「96×72 的畫面 ｜ 那一小塊（已經是影片的大小）」，ffmpeg 把左邊放大成 960×720，再把右邊那一塊原樣貼回左上角。
// 那一小塊是從 sequence_video::frame_image 切下來的，所以貼上去之後就是那一格該有的樣子。
void App::export_sequence(const std::vector<ReceivedFrame>& frames, const std::string& path, std::function<void(bool)> completion) {
    if (frames.empty() || exporting_video) return;
    const std::string ffmpeg = FileSource::ffmpeg_path();
    if (ffmpeg.empty()) {
        notice = LF("影片存檔失敗：%@", {L("這台電腦沒有裝 ffmpeg")});
        if (completion) completion(false);
        return;
    }
    exporting_video = true;
    const VideoTimeline timeline = video_timeline(frames, settings.stamp_video_snr);
    const bool wanted = settings.stamp_video_snr;
    io_worker_->post([this, timeline, wanted, path, ffmpeg, completion] {
        const double fps = sequence_video::kFrameRate;
        const std::vector<VideoPicture>& pictures = timeline.pictures;
        // 要印的字佔影片畫面的哪一塊：每一張的字不一樣長，取最寬的。邊界對齊到偶數的像素（影片的色度是兩個像素一格）
        int left = 0, top = 0, right = 0, bottom = 0;
        if (wanted) {
            for (const VideoPicture& picture : pictures) {
                const VideoStamp::Rect rect = VideoStamp::rect_for(picture.label);
                if (rect.width <= 0 || rect.height <= 0) continue;
                if (right == 0) {
                    left = rect.x;
                    top = rect.y;
                }
                left = std::min(left, rect.x);
                top = std::min(top, rect.y);
                right = std::max(right, rect.x + rect.width);
                bottom = std::max(bottom, rect.y + rect.height);
            }
            left -= left % 2;
            top -= top % 2;
            right += right % 2;
            bottom += bottom % 2;
        }
        const int patch_width = right - left, patch_height = bottom - top;
        // 找不到字型（或那一塊放不進畫面）時印不了：影片照存，結果裡說明沒有印
        const bool stamping = wanted && patch_width > 0 && patch_height > 0 && right <= sequence_video::kWidth && bottom <= sequence_video::kHeight;
        const int frame_width = kImageWidth + (stamping ? patch_width : 0), frame_height = stamping ? std::max(kImageHeight, patch_height) : kImageHeight;

        Process::Options options;
        options.args = {ffmpeg, "-y", "-nostdin", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                        "-s", std::to_string(frame_width) + "x" + std::to_string(frame_height), "-r", std::to_string(sequence_video::kFrameRate), "-i", "pipe:0"};
        const std::string size = std::to_string(sequence_video::kWidth) + ":" + std::to_string(sequence_video::kHeight);
        if (stamping) {
            const std::string graph = "[0:v]split[picture][corner];[picture]crop=" + std::to_string(kImageWidth) + ":" + std::to_string(kImageHeight) + ":0:0,scale=" + size +
                                      ":flags=neighbor[large];[corner]crop=" + std::to_string(patch_width) + ":" + std::to_string(patch_height) + ":" + std::to_string(kImageWidth) +
                                      ":0[stamp];[large][stamp]overlay=" + std::to_string(left) + ":" + std::to_string(top) + "[out]";
            options.args.insert(options.args.end(), {"-filter_complex", graph, "-map", "[out]"});
        } else {
            options.args.insert(options.args.end(), {"-vf", "scale=" + size + ":flags=neighbor"});
        }
        options.args.insert(options.args.end(), {"-pix_fmt", "yuv420p", "-movflags", "+faststart", path});
        options.pipe_stdin = true;
        options.pipe_stderr = true;
        std::string reason;
        std::unique_ptr<Process> process = Process::spawn(options, &reason);
        bool ok = process != nullptr;
        long written = 0;
        if (ok) {
            std::vector<std::uint8_t> packed(static_cast<std::size_t>(frame_width) * static_cast<std::size_t>(frame_height) * 3, 0);
            // 每一張要佔幾格：照它和下一張相隔的時間，四捨五入到整格（至少一格）；累計起來算，誤差才不會一路加上去
            for (std::size_t i = 0; i < pictures.size() && ok; ++i) {
                if (!pictures[i].picture) continue;
                const double end = i + 1 < pictures.size() ? pictures[i + 1].start : pictures[i].start + timeline.last_hold;
                const long target = std::max<long>(written + 1, std::lround(end * fps));
                const std::uint8_t* data = pictures[i].picture->rgb.data();
                if (stamping) {
                    // 左邊是 96×72 的畫面，右邊是影片那一格（印好字的）左上角的那一塊
                    const std::vector<std::uint8_t> full = sequence_video::frame_image(*pictures[i].picture, pictures[i].label);
                    for (int y = 0; y < kImageHeight; ++y) {
                        std::copy_n(data + static_cast<std::size_t>(y) * kImageWidth * 3, kImageWidth * 3, packed.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * static_cast<std::size_t>(frame_width) * 3));
                    }
                    for (int y = 0; y < patch_height; ++y) {
                        std::copy_n(full.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(top + y) * sequence_video::kWidth + static_cast<std::size_t>(left)) * 3), patch_width * 3,
                                    packed.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y) * static_cast<std::size_t>(frame_width) + kImageWidth) * 3));
                    }
                    data = packed.data();
                }
                // 這一張顯示到下一張開始為止：同一格畫面重複送，影片的格率才是固定的每秒 24 格
                for (; written < target && ok; ++written) ok = write_exact(process->in(), data, stamping ? packed.size() : static_cast<std::size_t>(kRgbBytes));
            }
            process->close_stdin();
            int code = 0;
            ok = process->wait(120.0, &code) && code == 0 && ok;
            // 結束碼在 Windows 上拿不到（見 process.hpp）：另外確認檔案真的寫出來了
            SDL_PathInfo info{};
            ok = ok && SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE && info.size > 0;
            if (!ok) reason = trim(read_all(process->err()));
        }
        const int count = static_cast<int>(pictures.size());
        const double seconds = static_cast<double>(written) / fps;
        const bool stamped = stamping || !wanted;
        post_main([this, ok, stamped, reason, path, count, seconds, completion] {
            exporting_video = false;
            if (!ok) notice = LF("影片存檔失敗：%@", {reason.empty() ? path : reason});
            else if (!stamped) notice = LF("影片已存成「%@」，但找不到字型，沒有印上訊雜比", {base_name(path)});
            else notice = LF("影片已存成「%@」：%d 張，%@", {base_name(path), count, clock_text(seconds)});
            if (completion) completion(ok);
        });
    });
}

// ---------------------------------------------------------------- 發射來源

void App::set_source_kind(SourceKind kind) {
    if (settings.source_kind == kind) return;
    schedule_restore_source_.reset();   // 定時發射中使用者自己換了來源：結束後就不再換回去
    settings.source_kind = kind;
    restart_source();
}

void App::select_camera(const std::string& name) {
    settings.camera_name = name;
    restart_source();
}

/// 目前選的鏡頭名稱。
std::string App::camera_name() const {
    for (const CameraInfo& camera : cameras) {
        if (camera.name == settings.camera_name) return camera.name;
    }
    return cameras.empty() ? L("沒有鏡頭") : cameras.front().name;
}

void App::restart_source() {
    if (!live) return;
    source_message.reset();
    clear_source_frame();
    switch (settings.source_kind) {
    case SourceKind::camera:
        if (!opening_video_) video_.pause();
        video_playing = false;
        start_camera();
        break;
    case SourceKind::video:
        camera_.stop();
        if (opening_video_) break;   // 開完會自己接手
        if (!video_.is_open()) {
            source_message = L("還沒選擇影片檔");
        } else if (settings.loop_video && !video_.is_image()) {
            video_.play();   // 不循環的影片停在原來的位置，按下發射才開始播（見 go_on_air）
        }
        break;
    case SourceKind::images:
        camera_.stop();
        if (!opening_video_) video_.pause();
        video_playing = false;
        // 預覽（下一張要送的那一張）由 ui_tick 讀進來（見 refresh_image_preview）
        if (image_files.empty()) source_message = L("還沒選擇圖片");
        break;
    }
}

/// 畫布目前的圖層；沒有打開（或沒有字）時是空的。設定改了就在這裡重算一次（不到一毫秒）。
std::shared_ptr<const CanvasLayer> App::current_canvas() {
    const std::optional<CanvasStyle> style = settings.canvas_style();
    if (!canvas_ever_built_ || style != canvas_built_) {
        canvas_ever_built_ = true;
        canvas_built_ = style;
        canvas_layer_ = style ? CanvasLayer::make(*style) : nullptr;
        ++canvas_version_;
    }
    return canvas_layer_;
}

Picture App::apply_canvas(const Picture& picture) {
    const std::shared_ptr<const CanvasLayer> layer = current_canvas();
    return layer ? layer->apply(picture) : picture;
}

/// 打開、關掉畫布。第一次打開而還沒有填字的時候，先填上「CQ 呼號」。
void App::set_canvas_enabled(bool on) {
    if (on && trim(settings.canvas_text).empty()) settings.canvas_text = settings.callsign.empty() ? "CQ" : "CQ " + settings.callsign;
    settings.canvas_enabled = on;
}

/// 換了來源（或換了影片檔）：上一個來源留下的畫面不能再顯示，也不能再拿來發射。
void App::clear_source_frame() {
    ++source_epoch_;
    latest_frame_.reset();
    ++latest_version_;
    video_frame_version_ = static_cast<std::uint64_t>(-1);
    preview_version_ = static_cast<std::uint64_t>(-1);
    image_preview_key_.reset();
    last_source_rgb_.reset();
    source_preview.reset();
    source_size.reset();
}

void App::start_camera() {
    // 開發用：設了 NBTV_NO_DEVICES 就完全不開鏡頭
    if (std::getenv("NBTV_NO_DEVICES") != nullptr) {
        source_message = L("沒有開鏡頭（NBTV_NO_DEVICES）");
        return;
    }
    if (const std::optional<std::string> error = camera_.start(settings.camera_name)) source_message = *error;
}

/// 上次選的影片檔還在的話，啟動時自動開啟：定時發射播的就是它，不必每次開 app 都重選。
void App::reopen_last_video() {
    if (settings.video_path.empty() || video_.is_open() || opening_video_) return;
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(settings.video_path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return;
    open_video(settings.video_path, false);
}

// autoplay 沒有給時照「循環播放」的設定：循環的影片開了就播；不循環的停在開頭，按下發射才開始播、播完自動停止發射。
// 來源不是影片檔的時候（從定時發射的面板選影片）只開啟，不播放。
void App::open_video(const std::string& path, bool remember, std::optional<bool> autoplay, std::function<void(bool)> completion) {
    if (opening_video_) return;
    if (settings.source_kind == SourceKind::video) {
        source_message.reset();
        clear_source_frame();
    }
    video_.set_loop(settings.loop_video && !scheduled_run);
    opening_video_ = true;
    video_playing = false;
    io_worker_->post([this, path, remember, autoplay, completion] {
        const std::optional<std::string> error = video_.open(path);
        post_main([this, path, remember, autoplay, completion, error] {
            opening_video_ = false;
            if (!error) {
                video_name = base_name(path);
                video_is_image = video_.is_image();
                video_duration = video_.duration();
                video_position = 0;
                video_frame_version_ = static_cast<std::uint64_t>(-1);
                if (remember) settings.video_path = path;   // 使用者自己選的記下來，下次啟動時自動開啟
                const bool is_source = settings.source_kind == SourceKind::video;
                if (!video_is_image && autoplay.value_or(settings.loop_video && is_source)) video_.play();
            } else {
                video_name.reset();
                video_is_image = false;
                video_duration = 0;
                // 來源是鏡頭時，預覽那一格是鏡頭的地方，訊息改放底列
                if (settings.source_kind == SourceKind::video) source_message = *error;
                else notice = LF("無法播放影片檔「%@」", {base_name(path)});
            }
            if (completion) completion(!error);
        });
    });
}

void App::toggle_video_playback() {
    if (!video_.is_open() || opening_video_ || video_is_image) return;
    if (video_.is_playing()) video_.pause();
    else video_.play();
    video_playing = video_.is_playing();
}

void App::scrub_video(double seconds, bool editing) {
    if (!video_.is_open() || opening_video_ || video_is_image) return;
    scrubbing_ = editing;
    video_position = seconds;
    // 每移一次都要 ffmpeg 重新找位置：拖著的時候最多每 0.15 秒一次，放開時再對準一次
    const double now = uptime_seconds();
    if (editing && now - last_seek_time_ < 0.15) return;
    last_seek_time_ = now;
    video_.seek(seconds);
}

// ---------------------------------------------------------------- 發射來源：圖片序列
//
// 使用者選一批圖片（或一個資料夾），照檔名排好（見 ImageSequence::files）。image_index 是「下一張要送的」，
// 右側欄的預覽就是它。發射時每準備一張畫面就取這一張、位置往後移一張；沒有勾循環播放的話，
// 最後一張排進輸出佇列之後自動收尾（和影片播完一樣），位置回到第一張。
// 取走了卻沒有完整送出去的那一張會退回去，下一次發射從它開始，所以不會有哪一張被跳過：
// 按停止時已經排進去、還沒開始播的（stop_transmit）；送到一半被切掉的、還沒開始送就取消的（end_transmit）。
// 讀圖在 preview_worker_ 上做。讀不到的檔案從序列裡拿掉，並在底列說明。

int App::open_images(const std::vector<std::string>& selection, bool remember) {
    std::vector<std::string> files = ImageSequence::files(selection);
    if (files.empty()) {
        notice = L("選的項目裡沒有可以讀取的圖片");
        return 0;
    }
    set_images(std::move(files));
    // 使用者自己選的這一批記下來，下次啟動時自動帶回來（見 reopen_last_images）
    if (remember) settings.image_paths = image_files;
    return static_cast<int>(image_files.size());
}

void App::set_images(std::vector<std::string> files) {
    image_files = std::move(files);
    image_index = 0;
    preview_worker_->post([this] { images_.forget(); });
    note("圖片序列：" + std::to_string(image_files.size()) + " 張，第一張「" + base_name(image_files.front()) + "」");
    if (settings.source_kind == SourceKind::images) {
        source_message.reset();
        clear_source_frame();
    }
}

/// 上次選的那一批圖片還在的話，啟動時自動帶回來（已經不在的檔案略過）。
void App::reopen_last_images() {
    if (!image_files.empty()) return;
    std::vector<std::string> files;
    for (const std::string& path : settings.image_paths) {
        SDL_PathInfo info{};
        if (SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE) files.push_back(path);
    }
    if (!files.empty()) set_images(std::move(files));
}

void App::select_image(int index) {
    if (image_files.empty()) return;
    image_index = std::clamp(index, 0, static_cast<int>(image_files.size()) - 1);
}

std::optional<std::string> App::next_image() const {
    if (image_index < 0 || image_index >= static_cast<int>(image_files.size())) return std::nullopt;
    return image_files[static_cast<std::size_t>(image_index)];
}

void App::open_dropped(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    bool folder = false;
    std::string first_file;   // 第一個不是資料夾的
    for (const std::string& path : paths) {
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo(path.c_str(), &info)) continue;
        if (info.type == SDL_PATHTYPE_DIRECTORY) folder = true;
        else if (first_file.empty()) first_file = path;
    }
    // 拖進來的是不是一批圖片：有資料夾、不只一個檔案，或來源已經是圖片序列而拖進來的是圖片
    const bool sequence = folder || paths.size() > 1 || (settings.source_kind == SourceKind::images && ImageSequence::is_image(paths.front()));
    if (sequence && !ImageSequence::files(paths).empty()) {
        set_source_kind(SourceKind::images);
        open_images(paths, true);
        return;
    }
    if (first_file.empty()) {
        // 拖進來的只有資料夾，裡面沒有圖片：來源不動，在底列說明
        notice = L("選的項目裡沒有可以讀取的圖片");
        return;
    }
    // 其餘的當成在「影片檔」那裡選了它（影片或圖片）；拖了好幾個不是圖片的檔案時用第一個
    set_source_kind(SourceKind::video);
    open_video(first_file, true);
}

/// 圖片序列的預覽：「下一張要送的」那一張縮成 96×72、印上畫布的樣子。位置、縮放方式或畫布變了就重算。
/// 一次只算一張：還在算的時候位置又變了（拖著滑桿），等這一張算完再算最新的那一張，不會越積越多。
void App::refresh_image_preview() {
    if (image_preview_loading_) return;
    const std::optional<std::string> path = next_image();
    if (!path) return;
    const std::shared_ptr<const CanvasLayer> canvas = current_canvas();
    const ImagePreviewKey key{*path, settings.scale_mode, canvas_version_};
    if (image_preview_key_ && key == *image_preview_key_) return;
    image_preview_loading_ = true;
    const int epoch = source_epoch_;
    // 順便把再下一張也先讀好：發射中輪到它的時候不必等（幾千萬像素的相片要讀一陣子）
    const std::string ahead = image_files[static_cast<std::size_t>(image_index + 1) % image_files.size()];
    preview_worker_->post([this, key, canvas, epoch, ahead] {
        const SourceFramePtr frame = images_.picture(key.path);
        std::optional<Picture> scaled;
        PicturePtr shown;
        if (frame) {
            scaled = scale_frame(*frame, key.scale);
            shown = std::make_shared<Picture>(canvas ? canvas->apply(*scaled) : *scaled);
        }
        post_main([this, key, epoch, frame, scaled, shown] {
            image_preview_loading_ = false;
            if (epoch != source_epoch_) return;   // 這段時間裡換了來源，或換了一批圖片
            if (!frame) {
                remove_unreadable_images({key.path});   // 下一次 ui_tick 改算接替它的那一張
                return;
            }
            image_preview_key_ = key;
            last_source_rgb_ = scaled;
            source_preview = shown;
            source_size = std::make_pair(frame->shown_width(), frame->shown_height());
        });
        if (ahead != key.path) images_.picture(ahead);
    });
}

/// 讀不到的圖片（檔案不見了、壞了）從序列裡拿掉，並在底列說明。
/// 位置留在原來那一張上；原來那一張就是被拿掉的，那就是它後面的那一張（後面沒有了就回到第一張）。
void App::remove_unreadable_images(const std::vector<std::string>& unreadable) {
    std::vector<std::string> gone;
    for (const std::string& path : unreadable) {
        if (std::find(image_files.begin(), image_files.end(), path) != image_files.end() && std::find(gone.begin(), gone.end(), path) == gone.end()) gone.push_back(path);
    }
    if (gone.empty()) return;
    const auto is_gone = [&gone](const std::string& path) { return std::find(gone.begin(), gone.end(), path) != gone.end(); };
    int kept_before = 0;
    for (int i = 0; i < image_index && i < static_cast<int>(image_files.size()); ++i) {
        if (!is_gone(image_files[static_cast<std::size_t>(i)])) ++kept_before;
    }
    image_files.erase(std::remove_if(image_files.begin(), image_files.end(), is_gone), image_files.end());
    image_index = kept_before < static_cast<int>(image_files.size()) ? kept_before : 0;
    if (gone.size() == 1) notice = LF("無法讀取圖片「%@」，已從序列中拿掉", {base_name(gone.front())});
    else notice = LF("有 %d 張圖片無法讀取，已從序列中拿掉", {static_cast<int>(gone.size())});
    note("圖片序列：拿掉讀不到的 " + std::to_string(gone.size()) + " 張（" + base_name(gone.front()) + (gone.size() > 1 ? "…" : "") + "），剩 " + std::to_string(image_files.size()) + " 張");
    if (image_files.empty() && settings.source_kind == SourceKind::images) {
        clear_source_frame();
        source_message = L("選的圖片都無法讀取");
    }
}

/// 圖片序列：讀「下一張要送的」那一張，縮成 96×72。讀不到的從序列裡拿掉，改讀它後面那一張
/// （勾了循環播放的話，找到最後一張還沒有，再從第一張找）。都讀不到時 rgb 是空的、last 是 true。
/// 位置在這一格確定要用的時候才往後移（見 accept）：取了之後才發現這次發射已經取消的話，這一張就不算取走。
void App::capture_next_image(std::function<void(SourceCapture)> completion) {
    std::vector<std::string> candidates;
    if (!image_files.empty()) {
        const std::ptrdiff_t start = std::clamp<std::ptrdiff_t>(image_index, 0, static_cast<std::ptrdiff_t>(image_files.size()) - 1);
        candidates.assign(image_files.begin() + start, image_files.end());
        if (settings.loop_images) candidates.insert(candidates.end(), image_files.begin(), image_files.begin() + start);
    }
    const ScaleMode mode = settings.scale_mode;
    preview_worker_->post([this, candidates, mode, completion] {
        std::vector<std::string> unreadable;
        SourceCapture capture;
        for (const std::string& path : candidates) {
            if (const SourceFramePtr frame = images_.picture(path)) {
                capture.rgb = scale_frame(*frame, mode);
                capture.image = path;
                break;
            }
            unreadable.push_back(path);
        }
        capture.last = !capture.rgb.has_value();
        post_main([this, unreadable, capture, completion] {
            remove_unreadable_images(unreadable);
            // 後面沒有讀得到的了：這一輪到此為止，位置回到第一張
            if (!capture.rgb) image_index = 0;
            completion(capture);
        });
    });
}

/// 取到的這一格確定要拿去編碼了。來源是圖片序列時：位置往後移一張（下一次取到的是下一張），
/// 並記下這一張還沒排進輸出佇列（見 put_back_image）。
/// 回傳來源是不是到這一格為止（影片播完了；不循環的圖片序列送到最後一張）：是的話，這一張排進去之後就收尾。
bool App::accept(const SourceCapture& capture) {
    if (!capture.image) return capture.last;
    tx_pending_image_ = capture.image;
    const auto found = std::find(image_files.begin(), image_files.end(), *capture.image);
    if (found == image_files.end()) return false;   // 這段時間裡換了一批圖片
    const std::size_t position = static_cast<std::size_t>(found - image_files.begin());
    const bool at_end = position + 1 >= image_files.size();
    image_index = at_end ? 0 : static_cast<int>(position) + 1;
    return at_end && !settings.loop_images;
}

/// 圖片序列：取走了卻沒有完整送出去的那一張退回去，下一次取到的就是它。
void App::put_back_image(const std::optional<std::string>& path) {
    if (!path) return;
    const auto found = std::find(image_files.begin(), image_files.end(), *path);
    if (found == image_files.end()) return;
    const int index = static_cast<int>(found - image_files.begin());
    if (index == image_index) return;
    image_index = index;
    note("圖片序列：「" + base_name(*path) + "」沒有送完，下一次從它開始");
}

/// 每 0.1 秒一次：把這段時間累積的結果一起反映到畫面上。
void App::ui_tick() {
    schedule_tick();   // 定時發射：到時間了嗎
    // 發射：送出去的訊號的頻譜，以及目前這張的進度
    if (!queued_monitor_rows_.empty()) {
        if (is_on_air()) append_spectrum(queued_monitor_rows_, WaterfallStore::Reference::transmit);
        queued_monitor_rows_.clear();
    }
    publish_transmit_display();
    if (tx_phase == TxPhase::idle) tx_mode_id = settings.tx_mode_id;

    // 發射來源
    if (settings.source_kind == SourceKind::camera && camera_.running()) {
        if (camera_.denied()) {
            source_message = L("沒有使用鏡頭的權限");
        } else if (SourceFramePtr frame = camera_.poll()) {
            latest_frame_ = frame;
            ++latest_version_;
        }
    }
    if (video_.is_open() && !opening_video_) {
        video_.set_loop(settings.loop_video && !scheduled_run);   // 定時發射中一律只播一次
        // 畫面只在來源是影片檔時才取；播放的位置與狀態隨時照實更新（來源是鏡頭時影片也可能開著，定時發射要用）
        if (settings.source_kind == SourceKind::video) {
            std::uint64_t version = 0;
            SourceFramePtr frame = video_.current_frame(&version);
            if (frame && version != video_frame_version_) {
                video_frame_version_ = version;
                latest_frame_ = frame;
                ++latest_version_;
            }
        }
        if (!scrubbing_) video_position = video_.position();
        video_playing = video_.is_playing();
    }
    if (settings.source_kind == SourceKind::images) refresh_image_preview();
    else refresh_preview();
}

void App::publish_transmit_display() {
    if (!is_on_air() || !tx_display_) return;
    if (tx_frame_number != tx_display_->number || !tx_preview) {
        tx_frame_number = tx_display_->number;
        tx_preview = tx_display_->preview;
    }
    tx_frames_sent = tx_display_->sent;
    tx_frame_progress = tx_display_->progress;
    tx_elapsed = tx_display_->elapsed;
}

/// 來源有新的一格（或縮放方式、畫布改了）時，重算 96×72 的預覽。預覽就是會送出去的樣子，畫布的字也印在上面。
void App::refresh_preview() {
    const SourceFramePtr frame = latest_frame_;
    const ScaleMode mode = settings.scale_mode;
    const std::shared_ptr<const CanvasLayer> canvas = current_canvas();
    const bool restyled = !preview_scale_ || mode != *preview_scale_ || canvas_version_ != preview_canvas_version_;
    if (!frame || (latest_version_ == preview_version_ && !restyled)) return;
    // 把整格來源畫面縮小並不便宜：來源一直在動時每秒更新 5 次就夠了；改縮放方式、改畫布或剛換來源時立刻更新
    const double now = uptime_seconds();
    if (!restyled && source_preview && now - preview_time_ < 0.19) return;
    preview_time_ = now;
    preview_version_ = latest_version_;
    preview_scale_ = mode;
    preview_canvas_version_ = canvas_version_;
    const int epoch = source_epoch_;
    preview_worker_->post([this, frame, mode, canvas, epoch] {
        const Picture scaled = scale_frame(*frame, mode);
        auto shown = std::make_shared<Picture>(canvas ? canvas->apply(scaled) : scaled);
        post_main([this, scaled, shown, epoch, width = frame->shown_width(), height = frame->shown_height()] {
            if (epoch != source_epoch_) return;   // 這段時間裡換了來源
            last_source_rgb_ = scaled;
            source_preview = shown;
            source_size = std::make_pair(width, height);
        });
    });
}

/// 取來源當下那一格（縮成 96×72，還沒有印上畫布的字）。鏡頭與影片檔沒有新的畫面時沿用上一格；
/// 圖片序列取的是「下一張要送的」那一張（見 capture_next_image）。取到之後確定要用，再呼叫 accept。
void App::capture_source_frame(std::function<void(SourceCapture)> completion) {
    if (settings.source_kind == SourceKind::images) {
        capture_next_image(std::move(completion));
        return;
    }
    // 來源的影片已經播完：這次取到的是它的最後一格
    const bool last = video_finished();
    if (settings.source_kind == SourceKind::video && video_.is_open() && !opening_video_) {
        std::uint64_t version = 0;
        SourceFramePtr frame = video_.current_frame(&version);
        if (frame && version != video_frame_version_) {
            video_frame_version_ = version;
            latest_frame_ = frame;
            ++latest_version_;
        }
    }
    const SourceFramePtr frame = latest_frame_;
    const ScaleMode mode = settings.scale_mode;
    const std::optional<Picture> fallback = last_source_rgb_;
    preview_worker_->post([this, frame, mode, fallback, last, completion] {
        SourceCapture capture;
        capture.rgb = frame ? std::optional<Picture>(scale_frame(*frame, mode)) : fallback;
        capture.last = last;
        post_main([completion, capture] { completion(capture); });
    });
}

bool App::can_transmit() const {
    if (demo_source_ready) return tx_phase == TxPhase::idle;
    return live && tx_phase == TxPhase::idle && (last_source_rgb_.has_value() || latest_frame_ != nullptr);
}

std::uint64_t App::demo_video_frame_version() const {
    std::uint64_t version = 0;
    return video_.current_frame(&version) ? version : 0;
}

bool App::demo_grab_video_frame() {
    const SourceFramePtr frame = video_.current_frame();
    if (!frame) return false;
    const Picture scaled = scale_frame(*frame, settings.scale_mode);
    last_source_rgb_ = scaled;
    source_preview = std::make_shared<Picture>(apply_canvas(scaled));
    source_size = std::make_pair(frame->shown_width(), frame->shown_height());
    video_position = video_.position();
    return true;
}

bool App::demo_load_image() {
    const std::optional<std::string> path = next_image();
    const SourceFramePtr frame = path ? ImageSequence::load(*path) : nullptr;
    if (!frame) return false;
    const Picture scaled = scale_frame(*frame, settings.scale_mode);
    last_source_rgb_ = scaled;
    source_preview = std::make_shared<Picture>(apply_canvas(scaled));
    source_size = std::make_pair(frame->shown_width(), frame->shown_height());
    return true;
}

void App::debug_set_source_frame(SourceFramePtr frame) {
    latest_frame_ = std::move(frame);
    ++latest_version_;
}

// ---------------------------------------------------------------- 發射
//
// 流程：取來源的一格 → 編碼（tx_worker_）→ 按下 PTT（ptt_worker_）→ 開輸出裝置、排入 0.3 秒靜音與第一張
// → 每張快播完時取來源當下那一格編下一張，緊接著排進去 → 按停止後送完目前這張、補上尾巴 → 放開 PTT → 回到接收。
// 這些步驟分散在幾條執行緒上，每一步回到主執行緒時都要先確認這次發射還沒被取消（tx_session_ 沒變）。

void App::toggle_transmit() {
    switch (tx_phase) {
    case TxPhase::idle:
        start_transmit();
        break;
    case TxPhase::starting:
    case TxPhase::transmitting:
        stop_transmit();
        break;
    case TxPhase::stopping:
        // 已經在收尾了又按一次：不等這一張送完，立刻停止
        if (uptime_seconds() - tx_stopping_at_ >= kAbortGuardSeconds) abort_transmit();
        break;
    }
}

/// 不等目前這張送完，立刻停止輸出、放開 PTT、回到接收。最後這一張不完整，對方解不出來。
/// 平常按停止是「送完目前這張」；這個是給等不及的時候用的（頻率不對、蓋到別人）。
void App::abort_transmit() {
    if (tx_phase != TxPhase::transmitting && tx_phase != TxPhase::stopping) return;
    note("立刻停止發射：最後一張沒有送完");
    end_transmit(L("按了立刻停止，最後一張沒有送完"));
}

void App::start_transmit(bool from_start) {
    const ModeInfo* mode = modes::find(settings.tx_mode_id);
    if (!live || !engine_ || tx_phase != TxPhase::idle || mode == nullptr) return;
    if (settings.callsign.empty()) {
        notice = L("還沒設定呼號，不能發射");
        return;
    }
    notice.reset();
    show_mode_menu = false;
    const int session = ++tx_session_;
    tx_phase = TxPhase::starting;
    tx_mode_id = mode->id;
    tx_elapsed = 0;
    tx_frames_sent = 0;
    tx_frame_number = 0;
    tx_frame_progress = 0;
    tx_preview.reset();
    on_air_.clear();
    tx_next_number_ = 1;
    tx_preparing_ = true;
    tx_tail_state_ = 0;
    tx_display_.reset();
    tx_last_rgb_.reset();
    tx_pending_image_.reset();
    tx_ends_after_first_ = false;
    const std::string callsign = settings.callsign;
    const int mode_id = mode->id;
    note("按下發射：模式 " + std::to_string(mode_id));
    const std::function<void()> encode_first = [this, session, callsign, mode_id] {
        if (session != tx_session_) return;
        capture_source_frame([this, session, callsign, mode_id](SourceCapture capture) {
            if (session != tx_session_) return;
            if (!capture.rgb) {
                fail_transmit(L("沒有來源畫面可以發射"));
                return;
            }
            // 圖片序列只剩這一張要送（沒有勾循環播放）：它排進去之後就收尾（見 go_on_air）。
            // 影片不會在第一張就播完（停在結尾的影片，下面已經先回到開頭了）
            const bool ended = accept(capture);
            tx_ends_after_first_ = ended && capture.image.has_value();
            tx_last_rgb_ = capture.rgb;
            const Picture picture = apply_canvas(*capture.rgb);   // 畫布的字在這裡印上去；tx_last_rgb_ 留的是沒有印字的
            tx_worker_->post([this, session, callsign, mode_id, picture] {
                // 上一次若有編好卻沒送出去的幀（取消、PTT 失敗），先取消它，再把重取樣濾波器清乾淨
                CoreTransmitter& transmitter = engine_->transmitter;
                transmitter.discard_last();
                transmitter.finish();
                transmitter.set_mode(mode_id);
                transmitter.set_callsign(callsign);
                const double began = uptime_seconds();
                std::optional<EncodedFrame> frame = transmitter.encode(picture);
                const double spent = uptime_seconds() - began;
                post_main([this, session, frame, spent] {
                    if (session == tx_session_) tx_prepare_seconds_ = spent;
                    first_frame_ready(frame, session);
                });
            });
        });
    };
    // 要不要先回到開頭：影片上一次播完停在結尾；或是指定要從頭來，而影片不是停在開頭
    const bool is_video = settings.source_kind == SourceKind::video && video_.is_open() && !opening_video_ && !video_is_image;
    const bool rewind = is_video && (video_.has_ended() || (from_start && (video_.is_playing() || video_.position() > 0.01)));
    if (rewind) {
        // 先回到開頭，等開頭那一格出來，第一張才不會是原來那個位置的畫面
        note(video_.has_ended() ? "影片停在結尾：回到開頭再發射" : "影片回到開頭再發射");
        std::uint64_t since = 0;
        video_.current_frame(&since);
        video_.rewind();
        wait_for_video_frame(session, since, 150, encode_first);
    } else {
        encode_first();
    }
}

/// 影片的位置剛移過（rewind）：新位置的那一格要一點時間才解得出來（要重新開一個 ffmpeg）。
/// 每 20 毫秒看一次，拿到了（或等了三秒）就繼續。since：位置移過去那時候影片畫面的版本。
void App::wait_for_video_frame(int session, std::uint64_t since, int attempts, std::function<void()> then) {
    if (session != tx_session_) return;
    std::uint64_t version = 0;
    SourceFramePtr frame = video_.current_frame(&version);
    if (frame && version != since) {
        video_frame_version_ = version;
        latest_frame_ = frame;
        ++latest_version_;
        then();
    } else if (attempts <= 0) {
        then();
    } else {
        after(0.02, [this, session, since, attempts, then] { wait_for_video_frame(session, since, attempts - 1, then); });
    }
}

/// 來源是只播一次的影片（沒有勾循環播放，或這一次是定時發射），而且已經播完、停在最後一格。
bool App::video_finished() const {
    return settings.source_kind == SourceKind::video && video_.is_open() && !video_is_image && (!settings.loop_video || scheduled_run) && video_.has_ended();
}

void App::debug_use_simulated_output(std::function<void(const std::vector<float>&)> sink, std::optional<double> stall_after) {
    if (!engine_) return;
    engine_->output.simulated = true;
    engine_->output.simulated_sink = std::move(sink);
    engine_->output.simulated_stall_after = stall_after;
}

void App::first_frame_ready(std::optional<EncodedFrame> frame, int session) {
    if (session != tx_session_ || tx_phase != TxPhase::starting) return;
    if (!frame) {
        fail_transmit(modes::find(tx_mode_id) == nullptr ? L("app 裡沒有這個模式的模型，不能發射") : L("畫面編碼失敗"));
        return;
    }
    note("第一張編好，按下 PTT（" + std::string(to_string(settings.ptt_mode)) + "）");
    // 先按下 PTT，再開始送音訊。driver 先登記起來：之後不論從哪裡取消，
    // 「放開」都會排在這個「按下」的後面（ptt_worker_ 一次只做一件），不會留下按著不放的 PTT。
    const std::shared_ptr<PTTDriver> driver = settings.make_ptt_driver();
    ptt_driver_ = driver;
    ptt_worker_->post([this, driver, frame, session] {
        if (const PTTResult error = driver->key(true)) {
            // 沒按成功也照樣放開一次。按下的指令可能其實到了電台（見 PTTError::may_be_keyed），放開也可能失敗：
            // 有一邊說不準，就要提醒使用者去看電台
            bool uncertain = error->may_be_keyed;
            if (driver->key(false)) uncertain = true;
            const std::string message = error->message;
            post_main([this, driver, session, uncertain, message] {
                if (ptt_driver_ == driver) ptt_driver_.reset();   // 上面已經放開了
                if (uncertain) warn_ptt_may_be_keyed(message);
                if (session != tx_session_ || tx_phase != TxPhase::starting) return;
                fail_transmit(LF("PTT 失敗：%@", {message}));
            });
            return;
        }
        post_main([this, frame, session] { go_on_air(*frame, session); });
    });
}

void App::go_on_air(EncodedFrame first, int session) {
    // 等 PTT 的這段時間裡已經取消了：PTT 由取消的那一邊放開，這裡什麼都不用做
    if (session != tx_session_ || tx_phase != TxPhase::starting || !engine_) return;
    engine_->stop_receiving();
    clear_sync_state();
    engine_->reset_spectrum();
    std::optional<AudioDevice> device;
    if (!engine_->output.simulated) {
        device = resolve_audio_device(settings.output_device, false);
        if (!device) {
            fail_transmit(L("找不到音訊輸出裝置"));
            return;
        }
    }
    engine_->output.gain = settings.output_gain();
    std::string error;
    if (!engine_->output.start(device ? device->id : 0, &error)) {
        fail_transmit(LF("無法開啟輸出裝置「%@」：%@", {device ? device->name : std::string("—"), error}));
        return;
    }
    if (device) output_device_name = device->name;
    tx_lead_ = static_cast<std::int64_t>(kPttLeadSeconds * kSampleRate);
    engine_->output.enqueue(std::vector<float>(static_cast<std::size_t>(tx_lead_), 0.0f));
    tx_end_ = tx_lead_;
    tx_monitored_ = tx_lead_;
    tx_pulled_frames_ = -1;
    tx_pulled_at_ = uptime_seconds();
    tx_phase = TxPhase::transmitting;
    if (!enqueue(first)) return;
    // 圖片序列只剩這一張：不再準備下一張，送完它就收尾
    if (tx_ends_after_first_) stop_after_queued_frames();
    // 來源是影片檔：開始送音訊的同時讓影片開始播放（已經在播的不動）。
    // 不循環的影片播完之後，再送出它的最後一格就自動停止（見 prepare_next_frame）。
    if (settings.source_kind == SourceKind::video && video_.is_open() && !opening_video_ && !video_is_image && !video_.is_playing()) {
        video_.play();
        video_playing = true;
        note("影片開始播放");
    }
    last_tx_tick_ = uptime_seconds();
    tx_tick();
    publish_transmit_display();   // 第一張的預覽馬上顯示，不等下一次 ui_tick
    note("開始送出音訊");
}

/// 把編好的一幀排進輸出佇列，接在目前排到的位置後面。
bool App::enqueue(const EncodedFrame& frame) {
    if (!engine_) return false;
    if (!engine_->output.enqueue(frame.samples)) {
        // 不應該發生：佇列的長度是照最長的模式配置的
        fail_transmit(L("輸出佇列放不下這一張，已停止發射"));
        return false;
    }
    OnAirFrame on_air;
    on_air.start = tx_end_;
    tx_end_ += static_cast<std::int64_t>(frame.samples.size());
    on_air.end = tx_end_;
    on_air.samples = frame.samples;
    on_air.preview = frame.preview;
    on_air.number = tx_next_number_;
    on_air.image = tx_pending_image_;
    tx_pending_image_.reset();
    on_air_.push_back(std::move(on_air));
    ++tx_next_number_;
    tx_preparing_ = false;
    return true;
}

double App::prepare_lead() const { return std::max(kPrepareLeadSeconds, tx_prepare_seconds_ * 1.5 + 0.25); }

void App::tx_tick() {
    if (!engine_ || (tx_phase != TxPhase::transmitting && tx_phase != TxPhase::stopping)) return;
    // 輸出裝置還有沒有來取取樣（佇列空了它也照樣來取，取到的是靜音）。USB 音效卡中途斷線、系統的音訊服務卡住時
    // 它就不來了：已經播到哪裡不會再前進，下面每一步（排下一張、收尾、定時發射的上限）都等不到。
    const std::int64_t pulled = engine_->output.rendered_frames();
    const double now = uptime_seconds();
    if (pulled != tx_pulled_frames_) {
        tx_pulled_frames_ = pulled;
        tx_pulled_at_ = now;
    } else if (now - tx_pulled_at_ > output_stall_seconds) {
        fail_transmit(L("音訊輸出裝置沒有回應（可能被拔掉了），已停止發射並放開 PTT"));
        return;
    }
    const std::int64_t position = engine_->output.played_samples();

    // 定時發射：超過上限還沒結束（影片卡住、中途被換成別的來源）就自己停下來，和按下停止一樣收尾
    if (scheduled_run && tx_phase == TxPhase::transmitting && schedule_limit_ && static_cast<double>(position - tx_lead_) / kSampleRate > *schedule_limit_) {
        schedule_limit_.reset();
        if (!schedule_limit_is_normal_) {
            schedule_cut_short_ = true;
            notice = LF("定時發射 %@ 超過影片的長度還沒結束，已自動停止", {schedule_label_});
        }
        note("定時發射到了上限：自動停止");
        stop_transmit();
    }

    // 把已經送出去的部分交給頻譜分析
    if (position > tx_monitored_) {
        std::vector<float> chunk;
        chunk.reserve(static_cast<std::size_t>(position - tx_monitored_));
        for (const OnAirFrame& frame : on_air_) {
            if (frame.end <= tx_monitored_ || frame.start >= position) continue;
            const std::int64_t from = std::max(tx_monitored_, frame.start) - frame.start;
            const std::int64_t to = std::min(position, frame.end) - frame.start;
            if (to > from) chunk.insert(chunk.end(), frame.samples.begin() + from, frame.samples.begin() + to);
        }
        tx_monitored_ = position;
        if (!chunk.empty()) engine_->monitor(std::move(chunk));
    }

    // 目前正在播的那一張（還在開頭的靜音時是第一張）。這裡只記下來，畫面上的數字由 ui_tick 更新。
    const OnAirFrame* current = nullptr;
    for (const OnAirFrame& frame : on_air_) {
        if (frame.start <= position) current = &frame;
    }
    if (current == nullptr && !on_air_.empty()) current = &on_air_.front();
    if (current != nullptr) {
        TransmitDisplay display;
        display.number = current->number;
        display.preview = current->preview;
        display.progress = static_cast<double>(std::max<std::int64_t>(0, std::min(position, current->end) - current->start)) / kSampleRate;
        display.sent = position >= current->end ? current->number : current->number - 1;
        display.elapsed = std::max(0.0, static_cast<double>(position - tx_lead_) / kSampleRate);
        tx_display_ = display;
    }
    while (on_air_.size() > 1 && on_air_.front().end <= position) on_air_.pop_front();

    if (tx_phase == TxPhase::transmitting) {
        // 目前這張快播完時，取來源當下那一格做下一張
        if (!tx_preparing_ && static_cast<double>(tx_end_ - position) / kSampleRate < prepare_lead()) prepare_next_frame();
    } else if (!tx_preparing_) {
        if (tx_tail_state_ == 0) queue_tail();
        else if (tx_tail_state_ == 2 && position >= tx_end_) finish_transmit();
    }
}

void App::prepare_next_frame() {
    tx_preparing_ = true;
    const int session = tx_session_;
    const double started = uptime_seconds();
    capture_source_frame([this, session, started](SourceCapture capture) {
        if (session != tx_session_) return;
        if (tx_phase != TxPhase::transmitting) {
            tx_preparing_ = false;
            return;
        }
        // 圖片序列已經沒有圖片可以送（剩下的都讀不到，底列有說明）：不重送上一張，目前這張送完就收尾
        if (!capture.rgb && capture.last) {
            tx_preparing_ = false;
            stop_after_queued_frames();
            return;
        }
        // 來源一時沒有畫面（剛切換來源）：重送上一格，不要讓電台空著發射
        const std::optional<Picture> rgb = capture.rgb ? capture.rgb : tx_last_rgb_;
        if (!rgb) {
            tx_preparing_ = false;
            return;
        }
        // 來源到這一格為止（影片播完了，這是它的最後一格；不循環的圖片序列送到最後一張）：這一張排進去之後就收尾
        const bool last_picture = accept(capture);
        tx_last_rgb_ = rgb;
        const Picture picture = apply_canvas(*rgb);   // 發射中改了畫布的字，下一張就照新的印
        tx_worker_->post([this, session, started, last_picture, picture] {
            std::optional<EncodedFrame> frame = engine_->transmitter.encode(picture);
            post_main([this, session, started, last_picture, frame] {
                if (session != tx_session_) return;
                // 下一次要提早多久：以這次從取畫面到編好的時間為準；比上次快的話慢慢降回來
                tx_prepare_seconds_ = std::max(uptime_seconds() - started, tx_prepare_seconds_ * 0.8);
                if (frame && tx_phase == TxPhase::transmitting) {
                    if (enqueue(*frame) && last_picture) stop_after_queued_frames();
                } else {
                    // 編好之後才發現已經按了停止：這張不送，發射機也回到編它之前的狀態（見 queue_tail）
                    if (frame) {
                        tx_worker_->post([this] { engine_->transmitter.discard_last(); });
                        note("停止時下一張剛編好：不送");
                    }
                    // 圖片序列：這一張沒有送出去，退回去（按停止的那一刻已經退過的話，這裡沒有東西可退）
                    put_back_image(tx_pending_image_);
                    tx_pending_image_.reset();
                    tx_preparing_ = false;
                }
            });
        });
    });
}

/// 停止發射：送完目前這張後放開 PTT、回到接收。
void App::stop_transmit() {
    switch (tx_phase) {
    case TxPhase::transmitting: {
        tx_phase = TxPhase::stopping;
        tx_stopping_at_ = uptime_seconds();
        pause_video_with_transmit();   // 之後不會再取新的畫面，影片不必等這一張送完才停
        const std::optional<OnAirFrame> dropped = drop_queued_frame();
        // 圖片序列：取走了卻不會送出去的退回去，下一次發射從它開始——已經排進佇列又被拿掉的那一張；
        // 沒有的話是正在編碼的那一張（兩張都有時，被拿掉的那一張在前面）
        put_back_image(dropped && dropped->image ? dropped->image : tx_pending_image_);
        tx_pending_image_.reset();
        if (!tx_preparing_) queue_tail();
        break;
    }
    case TxPhase::starting:
        // 還沒開始送：直接取消
        fail_transmit(std::nullopt);
        break;
    default:
        break;
    }
}

/// 來源的影片播完了（或圖片序列送到最後一張了），最後那一張也已經排進輸出佇列：和按下停止一樣收尾
/// （送完、補尾巴、放開 PTT、回到接收），差別是不拿掉已經排進去的那一張——對方要看得到結尾。
void App::stop_after_queued_frames() {
    if (tx_phase != TxPhase::transmitting) return;
    note(std::string(settings.source_kind == SourceKind::images ? "圖片序列送完" : "影片播完") + "：送完最後一張就停止發射");
    tx_phase = TxPhase::stopping;
    tx_stopping_at_ = uptime_seconds();
    if (!tx_preparing_) queue_tail();
}

/// 發射停下來了（按了停止，或送到一半出錯）：來源的影片跟著暫停。沒有在送的這段時間影片不會自己往前走，
/// 下一次發射從停下的地方接著播（見 go_on_air）。還沒開始送就取消的不算，影片維持原來的樣子。
void App::pause_video_with_transmit() {
    if (settings.source_kind != SourceKind::video || !video_.is_open() || opening_video_ || video_is_image) return;
    const bool was_playing = video_.is_playing();
    video_.pause();
    video_playing = false;
    if (!was_playing) return;
    if (!scrubbing_) video_position = video_.position();
    note("發射停止：影片跟著暫停");
}

/// 「送完目前這張」就停：下一張如果已經排進輸出佇列、但還沒開始播，把它拿掉。回傳拿掉的那一張；沒有拿掉任何東西時是空的。
std::optional<App::OnAirFrame> App::drop_queued_frame() {
    if (!engine_ || on_air_.empty()) return std::nullopt;
    if (!engine_->output.discard_queued(on_air_.back().start)) return std::nullopt;
    OnAirFrame next = std::move(on_air_.back());
    on_air_.pop_back();
    tx_end_ = next.start;
    tx_next_number_ = next.number;
    tx_worker_->post([this] { engine_->transmitter.discard_last(); });   // 發射機也回到編它之前的狀態（見 queue_tail）
    note("停止時下一張已經排進佇列但還沒開始播：拿掉");
    return next;
}

// 最後補上重取樣濾波器的尾巴與一小段靜音，確定整張都送出去了才放開 PTT。
//
// 核心的發射機在 48 kHz 時有一個串流式的重取樣濾波器：每一張輸出的最後約 0.5 毫秒留在濾波器裡，
// 接在下一張的開頭送出，最後一張的則由 nbtv_tx_finish 取出。如果有一張已經編好卻決定不送，
// 要先用 nbtv_tx_discard_last 取消它（見 drop_queued_frame 與 prepare_next_frame），
// 這裡取到的才是最後送出那一張的尾巴；一張都沒送的話，取到的是空的。
void App::queue_tail() {
    tx_tail_state_ = 1;
    const int session = tx_session_;
    tx_worker_->post([this, session] {
        std::vector<float> tail = engine_->transmitter.finish();   // 同時把發射機清回初始狀態
        post_main([this, session, tail] {
            if (session != tx_session_ || tx_phase != TxPhase::stopping || !engine_) return;
            // 尾巴要緊接在最後一張後面才有意義；佇列已經空了（來不及）就只補靜音
            const bool late = engine_->output.played_samples() >= tx_end_;
            std::vector<float> block = late ? std::vector<float>() : tail;
            block.insert(block.end(), static_cast<std::size_t>(kTailSilenceSeconds * kSampleRate), 0.0f);
            engine_->output.enqueue(block);
            tx_end_ += static_cast<std::int64_t>(block.size());
            tx_tail_state_ = 2;
        });
    });
}

void App::finish_transmit() {
    publish_transmit_display();
    note("發射結束：共送出 " + std::to_string(tx_frames_sent) + " 張");
    end_transmit(std::nullopt);
}

void App::fail_transmit(const std::optional<std::string>& message) {
    if (message) notice = *message;
    note(message ? "發射中止：" + *message : std::string("發射取消"));
    end_transmit(message ? *message : L("還沒開始送就取消了"));
}

/// 停止輸出、放開 PTT、回到接收。failure：不是正常送完的話，原因是什麼（定時發射記錄結果用）。
void App::end_transmit(const std::optional<std::string>& failure) {
    // 送到一半出錯才會在「發射中」直接走到這裡（按停止的話，當下就暫停過了；之後使用者自己又按播放的不再管）
    if (tx_phase == TxPhase::transmitting) pause_video_with_transmit();
    // 圖片序列：沒有完整送出去的第一張退回去，下一次發射從它開始——送到一半被切掉的（立刻停止、輸出裝置出問題）、
    // 排進去了還沒輪到的，或還沒開始送就取消的那一張。正常送完的話每一張都播完了，沒有東西要退。
    const std::int64_t played = engine_ ? engine_->output.played_samples() : 0;
    std::optional<std::string> unfinished = tx_pending_image_;
    for (const OnAirFrame& frame : on_air_) {
        if (frame.end > played && frame.image) {
            unfinished = frame.image;
            break;
        }
    }
    put_back_image(unfinished);
    tx_pending_image_.reset();
    ++tx_session_;
    tx_preparing_ = false;
    tx_last_rgb_.reset();
    tx_display_.reset();
    queued_monitor_rows_.clear();
    if (engine_) engine_->output.stop();
    release_ptt();
    tx_phase = TxPhase::idle;
    tx_mode_id = settings.tx_mode_id;
    on_air_.clear();
    tx_preview.reset();
    // 接收在開始送音訊時停掉了（還沒走到那一步就取消的話，接收一直沒停）
    if (engine_ && !engine_->is_receiving()) start_receiving();
    if (scheduled_run) finish_scheduled_run(failure);
}

void App::release_ptt() {
    if (!ptt_driver_) return;
    const std::shared_ptr<PTTDriver> driver = ptt_driver_;
    ptt_driver_.reset();
    ptt_worker_->post([this, driver] {
        if (const PTTResult error = driver->key(false)) {
            const std::string message = error->message;
            post_main([this, message] {
                notice = LF("放開 PTT 失敗：%@", {message});
                warn_ptt_may_be_keyed(message);
            });
        }
    });
}

// ---------------------------------------------------------------- 定時發射
//
// 開著的時候，每到時鐘上對齊整點的時刻（每 15 分鐘就是每小時的 00、15、30、45 分整），自動把影片檔從頭發射一次：
// 來源不是影片檔的話先換過去 → 影片回到開頭 → 走和按下發射一樣的流程 → 影片播完自動停止（不管有沒有勾循環播放）
// → 換回原來的來源。到了時間卻不能發射（正在發射、沒有影片、沒有呼號、正在收別人的畫面）就跳過那一次，
// 原因寫在底列與面板裡。來源是圖片時沒有「播完」，改成送固定的一段時間。
//
// 時刻由每 0.1 秒一次的 ui_tick 對照時鐘，不另外設計時器：電腦睡過頭、時鐘被調整的時候，比一下現在的時間
// 就知道那一次該不該發，醒來之後不會補發。

void App::set_schedule_armed(bool on) {
    if (on == schedule_armed) return;
    schedule_armed = on;
    schedule_countdown.reset();
    schedule_next_fire = on ? std::optional<double>(schedule::next_fire(clock(), settings.schedule_interval_minutes)) : std::nullopt;
    note(on ? "打開定時發射：下一次 " + schedule::clock_text(*schedule_next_fire) : std::string("關掉定時發射"));
    // 開著的時候請系統不要因為閒置而睡眠：睡著了，到時間就不會發射
    if (on && live && !sleep_inhibited_) {
        SDL_DisableScreenSaver();
        sleep_inhibited_ = true;
    } else if (!on && sleep_inhibited_) {
        SDL_EnableScreenSaver();
        sleep_inhibited_ = false;
    }
}

void App::reschedule_next() {
    if (!schedule_armed) return;
    schedule_countdown.reset();
    schedule_next_fire = schedule::next_fire(clock(), settings.schedule_interval_minutes);
}

void App::schedule_interval_changed() { reschedule_next(); }

/// 每 0.1 秒一次（ui_tick）：到時間了嗎？
void App::schedule_tick() {
    if (!schedule_armed || !schedule_next_fire) return;
    const double due = *schedule_next_fire;
    const double now = clock();
    const double remaining = due - now;
    const int interval = settings.schedule_interval_minutes;
    if (remaining > interval * 60.0 + 1) {
        reschedule_next();   // 時鐘被往回調了：從現在的時間重新算
        return;
    }
    if (remaining > 0) {
        schedule_countdown = remaining <= 60 ? std::optional<int>(static_cast<int>(std::ceil(remaining))) : std::nullopt;
        return;
    }
    schedule_countdown.reset();
    schedule_next_fire = schedule::next_fire(now, interval);
    const std::string label = schedule::clock_text(due);
    if (-remaining > kScheduleLateLimit) skip_schedule(label, L("錯過了時間（電腦剛從睡眠醒來，或時鐘被調整過）"));
    else fire_schedule(label);
}

/// 到時間了：把影片檔從頭發射一次。不能發射就跳過這一次。
void App::fire_schedule(const std::string& label) {
    if (tx_phase != TxPhase::idle) return skip_schedule(label, L("正在發射"));
    if (const std::optional<std::string> problem = schedule_problem()) return skip_schedule(label, *problem);
    if (settings.schedule_skip_when_busy && receiving_someone()) return skip_schedule(label, L("正在收別人的畫面"));
    const ModeInfo* mode = modes::find(settings.tx_mode_id);
    if (mode == nullptr) return skip_schedule(label, L("app 裡沒有這個發射模式的模型"));

    note("定時發射 " + label + "：開始");
    scheduled_run = true;
    schedule_label_ = label;
    schedule_cut_short_ = false;
    schedule_restore_source_.reset();
    if (settings.source_kind != SourceKind::video) {
        // 來源是鏡頭（或圖片序列）：這一次改用影片檔，結束後換回去（finish_scheduled_run）
        schedule_restore_source_ = settings.source_kind;
        settings.source_kind = SourceKind::video;
        source_message.reset();
        clear_source_frame();
        camera_.stop();
        // 影片目前位置的那一格
        std::uint64_t version = 0;
        if (SourceFramePtr frame = video_.current_frame(&version)) {
            video_frame_version_ = version;
            latest_frame_ = frame;
            ++latest_version_;
        }
    }
    video_.set_loop(false);   // 不管有沒有勾循環播放，定時發射只播一次；結束時改回來
    if (video_is_image) {
        schedule_limit_ = still_schedule_seconds;
        schedule_limit_is_normal_ = true;
    } else {
        schedule_limit_ = video_.duration() + 2 * mode->frame_seconds + schedule_margin_seconds;
        schedule_limit_is_normal_ = false;
    }
    start_transmit(true);
    // 不應該發生（能不能發射上面都檢查過了）：真的沒有開始的話，把狀態收乾淨
    if (tx_phase == TxPhase::idle) finish_scheduled_run(notice ? *notice : L("沒有開始"), 0);
}

/// 頻率上有人正在送這個 app 的訊號：接收機現在同步著，或 3 秒內還同步著
/// （連續送的時候，兩張之間同步會斷一下；只看這一瞬間的話，剛好落在空檔就會蓋到對方）。
bool App::receiving_someone() const {
    const double since = clock() - last_signal_time_;
    return synced || (since >= 0 && since < 3);
}

void App::skip_schedule(const std::string& label, const std::string& reason) {
    schedule_last_event = LF("%@ 沒有發射：%@", {label, reason});
    notice = LF("定時發射 %@ 沒有發射：%@", {label, reason});
    note("定時發射 " + label + " 沒有發射：" + reason);
}

/// 定時發射的那一次結束了（送完、被停止，或沒送成）：記下結果，把借用的東西還回去。
void App::finish_scheduled_run(const std::optional<std::string>& failure, std::optional<int> sent_override) {
    scheduled_run = false;
    schedule_limit_.reset();
    video_.set_loop(settings.loop_video);
    const int sent = sent_override.value_or(tx_frames_sent);
    if (failure) {
        schedule_last_event = sent > 0 ? LF("%@ 送出 %d 張後中止：%@", {schedule_label_, sent, *failure}) : LF("%@ 沒有發射：%@", {schedule_label_, *failure});
    } else {
        schedule_last_event = schedule_cut_short_ ? LF("%@ 送出 %d 張（超過影片的長度，自動停止）", {schedule_label_, sent}) : LF("%@ 送出 %d 張", {schedule_label_, sent});
    }
    note("定時發射 " + *schedule_last_event);
    if (schedule_restore_source_) {
        settings.source_kind = *schedule_restore_source_;
        schedule_restore_source_.reset();
        restart_source();
    }
}

std::optional<std::string> App::schedule_problem() const {
    if (settings.callsign.empty()) return L("還沒設定呼號");
    if (!video_name) return L("還沒選擇影片檔");
    return std::nullopt;
}

std::string App::schedule_summary() const {
    if (scheduled_run) return L("定時發射中");
    if (!schedule_armed || !schedule_next_fire) return L("定時發射");
    if (const std::optional<std::string> problem = schedule_problem()) return LF("定時發射：%@", {*problem});
    if (schedule_countdown) return LF("%d 秒後發射", {*schedule_countdown});
    return LF("定時 %@", {schedule::clock_text(*schedule_next_fire)});
}

// ---------------------------------------------------------------- 顯示用的文字

bool App::video_plays_once() const {
    return settings.source_kind == SourceKind::video && video_name.has_value() && !video_is_image && (!settings.loop_video || scheduled_run);
}

bool App::images_play_once() const { return settings.source_kind == SourceKind::images && !image_files.empty() && !settings.loop_images; }

std::string App::clock_text(double seconds) {
    const int total = static_cast<int>(std::max(seconds, 0.0));
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d", total / 60, total % 60);
    return text;
}

/// 音量條的長度：−50 dB 以下是空的，0 dB 是滿的。
double App::level_fraction(double db, double floor) { return std::min(std::max((db - floor) / -floor, 0.0), 1.0); }

/// 「12.3 dB」「−1.4 dB」：標在收到的畫面上的訊雜比，取到小數一位。
std::string App::snr_text(double db) {
    const long tenths = std::lround(std::fabs(db) * 10);
    return std::string(db < 0 && tenths > 0 ? "−" : "") + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " dB";
}

/// 「−18 dB」：負號用數學的減號，與版面稿一致。
std::string App::db_text(double db) {
    const long value = std::lround(db);
    return (value < 0 ? "−" + std::to_string(-value) : std::to_string(value)) + " dB";
}

}  // namespace nbtvgui
