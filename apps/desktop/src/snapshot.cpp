#include "snapshot.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>

#include "canvas.hpp"
#include "demo.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "offscreen.hpp"
#include "png.hpp"

namespace nbtvgui::snapshot {

namespace {

void say(const std::string& text) { log::line("[snapshot] " + text); }

/// 示範圖裡的裝置名稱：固定的，不看這台電腦上實際有什麼（算示範圖不碰任何裝置）。
void set_demo_devices(App& app) {
    app.input_device_name = "USB Audio CODEC";
    app.output_device_name = "USB Audio CODEC";
    app.input_devices = {AudioDevice{"USB Audio CODEC", 0}};
    app.output_devices = {AudioDevice{"USB Audio CODEC", 0}};
    app.serial_ports = {app.settings.serial_port};
    app.cameras = {CameraInfo{"USB Camera", 0}};
}

/// 右側欄的預覽：把一張測試圖當成來源的畫面（選的發射模式畫面比較大時，每個像素重複放大成送出的大小）。
void set_demo_source(App& app, const Picture& picture, int width, int height) {
    app.source_preview = std::make_shared<Picture>(app.apply_canvas(replicate_picture(picture, app.tx_picture_size())));
    app.source_size = std::make_pair(width, height);
    app.demo_source_ready = true;
}

void append(std::vector<float>& to, const std::vector<float>& from) { to.insert(to.end(), from.begin(), from.end()); }

}  // namespace

Settings demo_settings(SourceKind source) {
    Settings settings = Settings::load("");
    settings.callsign = "BX4ACP";
    settings.output_level_db = -6;
    settings.ptt_mode = PTTMode::serial;
#if defined(_WIN32)
    settings.serial_port = "COM3";
#elif defined(__APPLE__)
    settings.serial_port = "/dev/cu.usbserial-1420";
#else
    settings.serial_port = "/dev/ttyUSB0";
#endif
    settings.serial_line = SerialLine::rts;
    settings.rig_model = 3073;
    settings.rig_model_name = "Icom IC-7300";
    settings.serial_baud = 19200;
    settings.tx_mode_id = modes::default_id();
    settings.source_kind = source;
    settings.camera_name = "USB Camera";
    // 開發用：NBTV_SNAPSHOT_CANVAS=<文字> 打開畫布、印這些字；NBTV_SNAPSHOT_SNR=1 打開「在收到的畫面上標示訊雜比」
    if (const char* text = std::getenv("NBTV_SNAPSHOT_CANVAS"); text != nullptr && *text != '\0') {
        settings.canvas_text = text;
        settings.canvas_enabled = true;
    }
    settings.show_frame_snr = std::getenv("NBTV_SNAPSHOT_SNR") != nullptr;
    return settings;
}

double time_today(int hour, int minute, int second) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;
    return static_cast<double>(std::mktime(&local));
}

std::unique_ptr<App> make_receiving_app(bool quick) {
    auto app = std::make_unique<App>(demo_settings(SourceKind::camera), false);
    set_demo_devices(*app);
    const std::optional<Picture> coffee = demo::test_image("coffee"), rocket = demo::test_image("rocket"), cat = demo::test_image("cat");
    if (!coffee || !rocket || !cat) return app;
    const std::vector<double> times = {time_today(14, 19, 58), time_today(14, 20, 12), time_today(14, 20, 15), time_today(14, 32, 1), time_today(14, 32, 4), time_today(14, 32, 7)};

    if (quick) {
        // 不經過核心收發：直接放六張進「最近收到」，瀑布圖放幾秒的雜訊
        const int mode_ids[] = {12, 11, 11, 13, 13, 13};
        const Picture* pictures[] = {&*coffee, &*rocket, &*cat, &*coffee, &*rocket, &*cat};
        const int sequences[] = {0, 0, 1, 9, 10, 11};
        for (int i = 0; i < 6; ++i) {
            DecodedFrame frame;
            frame.mode_id = mode_ids[i];
            frame.decoded = true;
            frame.header_ok = true;
            frame.callsign = "N0CALL";
            frame.sequence = sequences[i];
            frame.snr_db = 13.2 + 0.3 * i;
            frame.picture = std::make_shared<Picture>(*pictures[i]);
            app->add(frame, times[static_cast<std::size_t>(i)]);
        }
        app->synced = true;
        app->sync_callsign = "N0CALL";
        app->snr_db = 14.1;
        app->offset_hz = 12.0;
        app->rx_mode_id = 13;
        app->input_level_db = -18;
        const std::vector<float> noise = demo::radio_channel(demo::silence(6), 0, std::nullopt, 0, -32, std::nullopt, 7);
        for (const auto& row : demo::monitor_rows(noise)) app->waterfall.append(row, WaterfallStore::Reference::noise);
        set_demo_source(*app, *coffee, 1280, 960);
        return app;
    }

    // 一次發射：同一部發射機連續送幾張。keep_from：前面幾張不收（當成還沒開始聽）；partial：最後一張只收前面這麼多個取樣
    const auto session = [](int mode, const std::vector<Picture>& images, std::size_t keep_from = 0, std::optional<std::size_t> partial = std::nullopt) {
        CoreTransmitter tx;
        tx.set_mode(mode);
        tx.set_callsign("N0CALL");
        std::vector<float> out;
        for (std::size_t index = 0; index < images.size(); ++index) {
            const std::optional<EncodedFrame> frame = tx.encode(images[index]);
            if (!frame || index < keep_from) continue;
            if (index + 1 == images.size() && partial) out.insert(out.end(), frame->samples.begin(), frame->samples.begin() + static_cast<std::ptrdiff_t>(std::min(*partial, frame->samples.size())));
            else append(out, frame->samples);
        }
        if (!partial) append(out, tx.finish());
        return out;
    };
    const std::vector<float> fast = session(12, {*coffee});
    const std::vector<float> medium = session(11, {*rocket, *cat});
    // 模式 13：共 13 張，只收第 10–12 張，以及第 13 張的前 61 個符號
    const Picture* cycle[] = {&*coffee, &*rocket, &*cat};
    std::vector<Picture> thirteen;
    for (int i = 0; i < 13; ++i) thirteen.push_back(*cycle[i % 3]);
    const ModeInfo* slow = modes::find(13);
    const std::size_t symbol_samples = slow != nullptr ? static_cast<std::size_t>(std::lround(slow->frame_seconds * kSampleRate / slow->frame_symbols)) : 1280;
    const std::vector<float> last = session(13, thirteen, 9, 61 * symbol_samples);
    std::vector<float> signal = fast;
    append(signal, medium);
    append(signal, last);
    std::vector<float> audio = demo::silence(0.6);
    append(audio, fast);
    append(audio, demo::silence(0.8));
    append(audio, medium);
    append(audio, demo::silence(6.5));
    append(audio, last);

    // 訊雜比 14 dB、電台偏了 +12 Hz、輸入音量約 −18 dB
    const std::vector<float> received = demo::radio_channel(audio, demo::power(signal), 14.0, 12.0, -32, -18.0);
    // 照實際接收的順序把每一塊的更新交給介面的狀態物件；時間用示範的鐘點
    std::size_t decoded = 0;
    RxUpdate final_update;
    for (const RxUpdate& update : demo::receive(received)) {
        RxUpdate status = update;
        status.frames.clear();
        app->apply(status);
        for (const DecodedFrame& frame : update.frames) {
            app->add(frame, times[std::min(decoded, times.size() - 1)]);
            if (frame.decoded) ++decoded;
        }
        final_update = update;
    }
    char text[200];
    std::snprintf(text, sizeof text, "接收：解出 %d 張（預期 %d 張），同步 %s，訊雜比 %.1f dB，頻偏 %+.1f Hz，輸入 %.1f dB", static_cast<int>(decoded), static_cast<int>(times.size()),
                  final_update.synced ? "是" : "否", final_update.snr_db, final_update.offset_hz, final_update.input_level_db);
    say(text);
    set_demo_source(*app, *coffee, 1280, 960);
    return app;
}

std::unique_ptr<App> make_transmitting_app(const std::deque<App::ReceivedFrame>& history, int mode_id) {
    Settings settings = demo_settings(SourceKind::camera);
    const ModeInfo* mode = modes::find(mode_id);
    if (mode != nullptr) settings.tx_mode_id = mode->id;
    auto app = std::make_unique<App>(settings, false);
    set_demo_devices(*app);
    app->history = history;
    if (mode == nullptr) return app;
    // 測試圖是 96×72；畫面比較大的模式每個像素重複放大
    const std::optional<Picture> coffee = demo::test_image("coffee", mode->picture), rocket = demo::test_image("rocket", mode->picture), cat = demo::test_image("cat", mode->picture);
    if (!coffee || !rocket || !cat) return app;
    // 共九張；瀑布圖只需要最後四張（前三張整張，加上目前這張的前 1.6 秒），所以只編那四張
    const Picture* cycle[] = {&*rocket, &*cat, &*coffee};
    app->tx_phase = App::TxPhase::transmitting;   // 先設好：畫布的字要照這個模式的畫面大小印（見 App::tx_picture_size）
    app->tx_mode_id = mode->id;
    CoreTransmitter tx;
    tx.set_mode(mode->id);
    tx.set_callsign(app->settings.callsign);
    std::vector<EncodedFrame> frames;
    for (int i = 5; i < 9; ++i) {
        if (std::optional<EncodedFrame> frame = tx.encode(app->apply_canvas(*cycle[i % 3]))) frames.push_back(std::move(*frame));
    }
    if (frames.empty()) {
        app->tx_phase = App::TxPhase::idle;
        return app;
    }
    const double progress = 1.6;
    app->tx_frame_number = 9;
    app->tx_frames_sent = 8;
    app->tx_frame_progress = progress;
    app->tx_elapsed = 8 * mode->frame_seconds + progress;
    app->tx_preview = frames.back().preview;
    std::vector<float> sent;
    for (std::size_t i = 0; i + 1 < frames.size(); ++i) append(sent, frames[i].samples);
    const std::vector<float>& current = frames.back().samples;
    sent.insert(sent.end(), current.begin(), current.begin() + static_cast<std::ptrdiff_t>(std::min(current.size(), static_cast<std::size_t>(progress * kSampleRate))));
    const float gain = app->settings.output_gain();
    for (float& sample : sent) sample *= gain;
    const auto rows = demo::monitor_rows(sent);
    for (const auto& row : rows) app->waterfall.append(row, WaterfallStore::Reference::transmit);
    say("發射：第 9 張（" + mode->label() + "），瀑布圖 " + std::to_string(rows.size()) + " 列");
    set_demo_source(*app, *coffee, 1280, 960);
    return app;
}

std::unique_ptr<App> make_fm_app(int tx_mode_id, bool quick) {
    // 哪幾個模式照核心的模式表分的組來挑，不寫死編號：有導頻的大畫面（現在是模式 15）、沒有導頻的大畫面（模式 4 到 7）
    const modes::MenuGroups groups = modes::menu_groups();
    if (groups.large.empty() || groups.pilot_free.empty()) return nullptr;
    const ModeInfo with_pilots = groups.large.front(), pilot_free = groups.pilot_free.front();
    Settings settings = demo_settings(SourceKind::camera);
    const ModeInfo* chosen = modes::find(tx_mode_id);
    settings.tx_mode_id = chosen != nullptr && chosen->large_picture() ? chosen->id : pilot_free.id;
    auto app = std::make_unique<App>(settings, false);
    set_demo_devices(*app);
    const std::optional<Picture> coffee = demo::test_image("coffee"), rocket = demo::test_image("rocket"), cat = demo::test_image("cat");
    if (!coffee || !rocket || !cat) return nullptr;
    const std::vector<double> times = {time_today(14, 31, 52), time_today(14, 31, 55), time_today(14, 32, 0), time_today(14, 32, 3), time_today(14, 32, 5)};
    const auto enlarged = [](const Picture& picture, const ModeInfo& mode) { return replicate_picture(picture, mode.picture); };

    if (quick) {
        const ModeInfo* modes_sent[] = {&with_pilots, &with_pilots, &pilot_free, &pilot_free, &pilot_free};
        const Picture* pictures[] = {&*coffee, &*rocket, &*cat, &*coffee, &*rocket};
        const int sequences[] = {0, 1, 0, 1, 2};
        for (int i = 0; i < 5; ++i) {
            DecodedFrame frame;
            frame.mode_id = modes_sent[i]->id;
            frame.decoded = true;
            frame.header_ok = true;
            frame.callsign = "N0CALL";
            frame.sequence = sequences[i];
            frame.snr_db = 24.2 + 0.3 * i;
            frame.picture = std::make_shared<Picture>(enlarged(*pictures[i], *modes_sent[i]));
            app->add(frame, times[static_cast<std::size_t>(i)]);
        }
        app->synced = true;
        app->sync_callsign = "N0CALL";
        app->snr_db = 25.0;
        app->offset_hz = 0.0;
        app->rx_mode_id = pilot_free.id;
        app->input_level_db = -18;
        const std::vector<float> noise = demo::radio_channel(demo::silence(6), 0, std::nullopt, 0, -32, std::nullopt, 7);
        for (const auto& row : demo::monitor_rows(noise)) app->waterfall.append(row, WaterfallStore::Reference::noise);
        set_demo_source(*app, *coffee, 1280, 960);
        return app;
    }

    // 同一部發射機一幀接一幀地送：先是有導頻的大畫面兩張，中間不停、直接換成沒有導頻的三張，第四張只送出前六成
    CoreTransmitter tx;
    tx.set_callsign("N0CALL");
    std::vector<float> signal;
    tx.set_mode(with_pilots.id);
    for (const Picture* picture : {&*coffee, &*rocket}) {
        if (const std::optional<EncodedFrame> frame = tx.encode(enlarged(*picture, with_pilots))) append(signal, frame->samples);
    }
    tx.set_mode(pilot_free.id);
    for (const Picture* picture : {&*cat, &*coffee, &*rocket}) {
        if (const std::optional<EncodedFrame> frame = tx.encode(enlarged(*picture, pilot_free))) append(signal, frame->samples);
    }
    if (const std::optional<EncodedFrame> frame = tx.encode(enlarged(*cat, pilot_free))) {
        signal.insert(signal.end(), frame->samples.begin(), frame->samples.begin() + static_cast<std::ptrdiff_t>(frame->samples.size() * 6 / 10));
    }
    std::vector<float> audio = demo::silence(0.6);
    append(audio, signal);

    // FM 語音通道的樣子：訊雜比 25 dB、沒有頻偏（FM 解調出來的音訊沒有頻偏；沒有導頻的幀也容不下頻偏）、輸入音量約 −18 dB
    const std::vector<float> received = demo::radio_channel(audio, demo::power(signal), 25.0, 0.0, -32, -18.0);
    std::size_t decoded = 0;
    RxUpdate final_update;
    for (const RxUpdate& update : demo::receive(received)) {
        RxUpdate status = update;
        status.frames.clear();
        app->apply(status);
        for (const DecodedFrame& frame : update.frames) {
            app->add(frame, times[std::min(decoded, times.size() - 1)]);
            if (frame.decoded) ++decoded;
        }
        final_update = update;
    }
    char text[240];
    std::snprintf(text, sizeof text, "FM 大畫面的接收：解出 %d 張（預期 %d 張），同步 %s，模式 %d，訊雜比 %.1f dB，頻偏 %+.1f Hz，輸入 %.1f dB；我們選的發射模式是模式 %d", static_cast<int>(decoded),
                  static_cast<int>(times.size()), final_update.synced ? "是" : "否", final_update.mode_id, final_update.snr_db, final_update.offset_hz, final_update.input_level_db, settings.tx_mode_id);
    say(text);
    set_demo_source(*app, *coffee, 1280, 960);
    return app;
}

std::unique_ptr<App> make_idle_app(const std::string& work_directory) {
    auto app = std::make_unique<App>(demo_settings(SourceKind::video), false);
    set_demo_devices(*app);
    const std::vector<float> noise = demo::radio_channel(demo::silence(22), 0, std::nullopt, 0, -32, std::nullopt, 7);
    int false_frames = 0;
    RxUpdate last;
    for (const RxUpdate& update : demo::receive(noise)) {
        false_frames += static_cast<int>(update.frames.size());
        app->apply(update);
        last = update;
    }
    char text[160];
    std::snprintf(text, sizeof text, "待機：同步 %s，誤解出 %d 張，輸入 %.1f dB", last.synced ? "是" : "否", false_frames, last.input_level_db);
    say(text);
    app->clock = [] { return time_today(14, 32, 7); };   // 示範的鐘點：下一次定時發射是 14:45

    // 影片檔來源：用測試圖做一段 38 秒的影片，照正常流程開啟、跳到 12 秒、取那一格。沒有 ffmpeg 時改用一張圖片
    const std::optional<Picture> rocket = demo::test_image("rocket");
    if (work_directory.empty() || !rocket) return app;
    std::string path = work_directory + "/rocket_launch.mp4";
    bool made = demo::make_video(path, *rocket);
    if (!made) {
        say("注意：示範影片做不出來（這台電腦沒有 ffmpeg？），改用一張圖片");
        path = work_directory + "/rocket_launch.png";
        made = demo::write_picture_png(*rocket, path, 10);
    }
    if (!made) return app;
    const auto opened = std::make_shared<std::optional<bool>>();
    app->open_video(path, false, false, [opened](bool ok) { *opened = ok; });
    demo::wait_until(app.get(), 30, [&] { return opened->has_value(); });
    if (opened->value_or(false)) {
        if (!app->video_is_image) {
            const std::uint64_t before = app->demo_video_frame_version();
            app->scrub_video(12, false);
            demo::wait_until(app.get(), 15, [&] { return app->demo_video_frame_version() != before; });
        } else {
            demo::wait_until(app.get(), 5, [&] { return app->demo_video_frame_version() != 0; });
        }
        const bool got = app->demo_grab_video_frame();
        app->demo_source_ready = got;
        say("影片檔：" + app->video_name.value_or("—") + "，長度 " + App::clock_text(app->video_duration) + "，取到畫面：" + (got ? "是" : "否"));
    } else {
        say("注意：示範影片無法開啟" + (app->source_message ? "（" + *app->source_message + "）" : std::string()));
    }
    return app;
}

std::unique_ptr<App> make_images_app(const std::string& work_directory) {
    Settings settings = demo_settings(SourceKind::images);
    // 開發用：NBTV_SNAPSHOT_LOOP_IMAGES=1 時勾著「循環播放」
    settings.loop_images = std::getenv("NBTV_SNAPSHOT_LOOP_IMAGES") != nullptr;
    auto app = std::make_unique<App>(settings, false);
    set_demo_devices(*app);
    if (work_directory.empty()) return app;
    // 開發用：NBTV_SNAPSHOT_IMAGES 給的是一個資料夾或圖片檔的路徑時，改用那裡的圖片（看這個程式怎麼讀某一批真的圖片，不必開視窗）
    std::string folder = work_directory + "/pictures";
    SDL_PathInfo info{};
    const char* given = std::getenv("NBTV_SNAPSHOT_IMAGES");
    if (given != nullptr && SDL_GetPathInfo(given, &info)) {
        folder = given;
    } else {
        SDL_CreateDirectory(folder.c_str());
        int number = 2041;
        for (const char* name : {"coffee", "rocket", "cat"}) {
            // 放大十倍，像一般的圖片（960×720）
            if (const std::optional<Picture> picture = demo::test_image(name)) demo::write_picture_png(*picture, folder + "/IMG_" + std::to_string(number) + ".png", 10);
            ++number;
        }
    }
    const int count = app->open_images({folder}, false);
    app->select_image(count > 1 ? 1 : 0);
    const bool got = app->demo_load_image();
    app->demo_source_ready = got;
    const std::optional<std::string> next = app->next_image();
    say("圖片序列：" + std::to_string(count) + " 張，下一張要送的是「" + (next ? next->substr(next->find_last_of("/\\") + 1) : std::string("—")) + "」，取到畫面：" + (got ? "是" : "否") +
        (app->source_size ? "，來源 " + std::to_string(app->source_size->first) + "×" + std::to_string(app->source_size->second) : std::string()));
    return app;
}

int run(const std::string& directory, const std::vector<std::string>& languages) {
    if (!SDL_CreateDirectory(directory.c_str())) {
        say("無法建立目錄 " + directory + "：" + SDL_GetError());
        return 1;
    }
    if (!demo::test_image("coffee") || modes::find(11) == nullptr || modes::find(12) == nullptr || modes::find(13) == nullptr) {
        say("少了內附的測試圖，或少了模式 11、12、13 的權重檔，無法產生示範資料");
        return 1;
    }
    const std::string original_language = i18n::language();
    i18n::use(languages.empty() ? original_language : languages.front());
    // 畫布的字型平常是介面載入字型時順便設定的；示範資料在介面之前就準備好了，所以這裡先設定一次，
    // 不然 NBTV_SNAPSHOT_CANVAS 要印的字在預覽與送出的畫面上印不出來
    CanvasLayer::set_fonts(find_fonts(i18n::language()).bold);
    const std::unique_ptr<App> receiving = make_receiving_app();
    const std::unique_ptr<App> transmitting = make_transmitting_app(receiving->history);
    const std::unique_ptr<App> idle = make_idle_app(directory);
    // 開發用，平常不算：
    //   NBTV_SNAPSHOT_IMAGES=1       多算一張「待機＋圖片序列來源」（extra-images.png）；同時設了 NBTV_SNAPSHOT_EXTRA_SIZE=寬x高 的話，
    //                               再多算一張那個視窗大小的（看縮到最小時右側欄放不放得下）。
    //                               值是一個資料夾或圖片檔的路徑時，序列用那裡的圖片，不用內附的測試圖
    //   NBTV_SNAPSHOT_SAVE_VIDEO=1   多算一張按了「儲存影片」之後、那一小塊選項開著的樣子（extra-save-video.png）
    //   NBTV_SNAPSHOT_SNR=1          另外把「存成影片時印上訊雜比」的一格存成 video-frame-snr.png（影片裡每一格就是這個樣子，960×720）
    //   NBTV_SNAPSHOT_FM=1           多算三張給 FM 用的大畫面（192×144）：接收中（extra-fm-receiving.png）、它的模式選單開著的樣子
    //                               （extra-fm-mode-menu.png）、發射中（extra-fm-transmitting.png）。值是某個大畫面模式的編號時，
    //                               我們這邊選的發射模式用它（不然是沒有導頻的那一組裡最快的）。同時設了 NBTV_SNAPSHOT_EXTRA_SIZE 的話，
    //                               接收中與選單那兩張再各多算一張那個視窗大小的
    const bool extra_images = std::getenv("NBTV_SNAPSHOT_IMAGES") != nullptr, extra_save_video = std::getenv("NBTV_SNAPSHOT_SAVE_VIDEO") != nullptr;
    const std::unique_ptr<App> images = extra_images ? make_images_app(directory) : nullptr;
    std::unique_ptr<App> fm, fm_transmitting;
    if (const char* wanted = std::getenv("NBTV_SNAPSHOT_FM")) {
        fm = make_fm_app(std::atoi(wanted));
        if (fm) fm_transmitting = make_transmitting_app(fm->history, fm->settings.tx_mode_id);
        else say("注意：app 裡沒有給 FM 用的大畫面模式（少了權重檔），那幾張沒有算");
    }
    int extra_width = 0, extra_height = 0;
    if (const char* size = std::getenv("NBTV_SNAPSHOT_EXTRA_SIZE")) std::sscanf(size, "%dx%d", &extra_width, &extra_height);
    // 影片的一格：一般的（96×72 放大十倍），有算 FM 那幾張的話再加一張 192×144 的（放大五倍，字的位置與大小相同）
    const auto video_frame = [&directory](const App& app, const std::string& name) {
        if (app.history.empty() || !app.history.front().picture) return;
        const App::ReceivedFrame& newest = app.history.front();
        const std::string label = "SNR " + App::snr_text(newest.snr_db);
        const std::vector<std::uint8_t> frame = sequence_video::frame_image(*newest.picture, label);
        if (write_png(directory + "/" + name, frame.data(), sequence_video::kWidth, sequence_video::kHeight, 3)) {
            say(name + "：影片的一格（畫面 " + newest.picture->size.text() + "），左上角印的是「" + label + "」" + (VideoStamp::has_fonts() ? "" : "　注意：找不到字型，沒有印出來"));
        }
    };
    if (std::getenv("NBTV_SNAPSHOT_SNR") != nullptr) {
        video_frame(*receiving, "video-frame-snr.png");
        if (fm) video_frame(*fm, "video-frame-snr-fm.png");
    }

    int failures = 0;
    for (const std::string& language : languages) {
        i18n::use(language);
        std::string out = directory;
        if (languages.size() > 1) {
            out += "/" + language;
            SDL_CreateDirectory(out.c_str());
        }
        const auto render = [&](App& app, const std::string& name, const std::function<void(UI&)>& prepare, int width = 1280, int height = 840) {
            OffscreenUI screen(app, width, height, 2.0f);
            if (!screen.ok()) {
                say(name + "：沒有算出來（" + screen.error() + "）");
                ++failures;
                return;
            }
            if (prepare) prepare(screen.ui());
            screen.frames(4);
            const std::string path = out + "/" + name;
            if (screen.save_png(path)) {
                say(language + "／" + name + "（" + std::to_string(screen.pixel_width()) + "×" + std::to_string(screen.pixel_height()) + "）" +
                    (screen.ui().has_cjk_fonts() ? "" : "　注意：找不到中文、日文的字型"));
            } else {
                say("無法寫入 " + path);
                ++failures;
            }
        };
        render(*receiving, "1-receiving.png", nullptr);
        render(*transmitting, "2-transmitting.png", nullptr);
        render(*idle, "3-idle-video.png", nullptr);
        render(*receiving, "4-settings.png", [](UI& ui) { ui.show_settings = true; });
        render(*receiving, "5-mode-menu.png", [](UI& ui) { ui.open_mode_menu(); });
        idle->set_schedule_armed(true);
        render(*idle, "6-schedule.png", [](UI& ui) { ui.open_schedule_menu(); });
        idle->set_schedule_armed(false);
        if (images) {
            render(*images, "extra-images.png", nullptr);
            if (extra_width > 0 && extra_height > 0) render(*images, "extra-images-" + std::to_string(extra_width) + "x" + std::to_string(extra_height) + ".png", nullptr, extra_width, extra_height);
        }
        if (extra_save_video) render(*receiving, "extra-save-video.png", [](UI& ui) { ui.open_save_video_menu(); });
        if (fm) {
            render(*fm, "extra-fm-receiving.png", nullptr);
            render(*fm, "extra-fm-mode-menu.png", [](UI& ui) { ui.open_mode_menu(); });
            if (extra_width > 0 && extra_height > 0) {
                const std::string size = std::to_string(extra_width) + "x" + std::to_string(extra_height);
                render(*fm, "extra-fm-receiving-" + size + ".png", nullptr, extra_width, extra_height);
                render(*fm, "extra-fm-mode-menu-" + size + ".png", [](UI& ui) { ui.open_mode_menu(); }, extra_width, extra_height);
            }
        }
        if (fm_transmitting) render(*fm_transmitting, "extra-fm-transmitting.png", nullptr);
    }
    i18n::use(original_language);
    say(failures == 0 ? "完成：" + directory : "有 " + std::to_string(failures) + " 張沒有算出來");
    return failures == 0 ? 0 : 1;
}

}  // namespace nbtvgui::snapshot
