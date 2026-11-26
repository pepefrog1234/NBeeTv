#include "selftest.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "app.hpp"
#include "audio.hpp"
#include "canvas.hpp"
#include "demo.hpp"
#include "fonts.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "model_file.hpp"
#include "net.hpp"
#include "offscreen.hpp"
#include "png.hpp"
#include "process.hpp"
#include "ptt.hpp"
#include "serial.hpp"
#include "settings.hpp"
#include "snapshot.hpp"
#include "sources.hpp"
#include "tasks.hpp"
#include "waterfall.hpp"

namespace nbtvgui::selftest {

namespace {

/// 一項測試裡用的：記下不對的地方、補充的資訊，或說明為什麼略過。
class Check {
public:
    void expect(bool ok, const std::string& what) {
        if (!ok) failures_.push_back(what);
    }
    void info(const std::string& text) { notes_.push_back(text); }
    /// 這台電腦上缺了什麼所以跑不了。needs：缺的是哪一樣（ffmpeg、hamlib、cjk、audio），CI 上用 NBTV_SELFTEST_REQUIRE 指定哪些不准缺。
    void skip(const std::string& why, const std::string& needs) {
        skipped_ = why;
        const char* required = std::getenv("NBTV_SELFTEST_REQUIRE");
        if (required != nullptr && ("," + std::string(required) + ",").find("," + needs + ",") != std::string::npos) failures_.push_back("這裡應該要有 " + needs + "，卻沒有：" + why);
    }
    const std::vector<std::string>& failures() const { return failures_; }
    const std::vector<std::string>& notes() const { return notes_; }
    const std::optional<std::string>& skipped() const { return skipped_; }

private:
    std::vector<std::string> failures_;
    std::vector<std::string> notes_;
    std::optional<std::string> skipped_;
};

struct Case {
    const char* name;
    const char* title;
    std::function<void(Check&)> body;
};

std::string number(double value, int digits = 2) {
    char text[64];
    std::snprintf(text, sizeof text, "%.*f", digits, value);
    return text;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool file_size(const std::string& path, std::uint64_t* size) {
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return false;
    if (size != nullptr) *size = info.size;
    return true;
}

/// 開發用：設了 NBTV_SELFTEST_SCREENS=<目錄> 時，把測試中途的幾個畫面存成 PNG
/// （跳出來的選單、滑鼠停著時的說明這些，示範圖裡看不到的）。
void keep_screen(const OffscreenUI& screen, const std::string& name) {
    const char* directory = std::getenv("NBTV_SELFTEST_SCREENS");
    if (directory == nullptr || *directory == '\0') return;
    SDL_CreateDirectory(directory);
    screen.save_png(std::string(directory) + "/" + name + ".png");
}

// ---------------------------------------------------------------- 測試用的 PTT 與輸出

struct PttLog {
    std::mutex mutex;
    std::vector<std::pair<bool, double>> events;   // 按下（true）或放開、發生的時刻
    /// 按下要花幾秒（像真的 PTT 要等電台回覆那樣）：這段時間裡發射停在「準備中」，用來測還沒開始送就取消
    std::atomic<double> press_delay{0.0};
    std::vector<bool> keys() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<bool> result;
        for (const auto& event : events) result.push_back(event.first);
        return result;
    }
    /// 同 keys，但取走之後清掉（同一個 app 接著測下一次發射）
    std::vector<bool> take() {
        std::vector<bool> result = keys();
        std::lock_guard<std::mutex> lock(mutex);
        events.clear();
        return result;
    }
};

/// 不碰任何硬體、只把每一次按下與放開記下來的 PTT。fail_press：按下時回報失敗（uncertain：而且說不準電台有沒有按下）。
class RecordingPTT : public PTTDriver {
public:
    RecordingPTT(std::shared_ptr<PttLog> log, bool fail_press, bool uncertain) : log_(std::move(log)), fail_press_(fail_press), uncertain_(uncertain) {}
    PTTResult key(bool on) override {
        const double delay = log_->press_delay.load();
        if (on && delay > 0) std::this_thread::sleep_for(std::chrono::duration<double>(delay));
        {
            std::lock_guard<std::mutex> lock(log_->mutex);
            log_->events.emplace_back(on, uptime_seconds());
        }
        if (on && fail_press_) {
            PTTError error{"模擬的 PTT 失敗"};
            error.may_be_keyed = uncertain_;
            return error;
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<PttLog> log_;
    bool fail_press_, uncertain_;
};

struct Sink {
    std::mutex mutex;
    std::vector<float> samples;
    double first_call = -1;
    std::vector<float> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return samples;
    }
    /// 同 take，但取走之後清掉（同一個 app 接著測下一次發射）
    std::vector<float> drain() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<float> taken;
        taken.swap(samples);
        return taken;
    }
};

/// 一個不碰裝置的 app：發射的聲音由模擬的輸出照實際的時間取走、存進 sink，PTT 只記錄。
/// 沒有呼叫 start()，所以不會去開音訊輸入、鏡頭。
struct Rig {
    std::shared_ptr<PttLog> ptt = std::make_shared<PttLog>();
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();
    std::unique_ptr<App> app;

    explicit Rig(int mode_id = 12, std::optional<double> stall_after = std::nullopt, bool fail_press = false, bool uncertain = false) {
        Settings settings = Settings::load("");
        settings.callsign = "BX4ACP";
        settings.tx_mode_id = mode_id;
        settings.output_level_db = 0;
        settings.source_kind = SourceKind::video;
        const std::shared_ptr<PttLog> log = ptt;
        settings.debug_ptt_driver = [log, fail_press, uncertain] { return std::make_unique<RecordingPTT>(log, fail_press, uncertain); };
        app = std::make_unique<App>(std::move(settings), true);
        const std::shared_ptr<Sink> target = sink;
        app->debug_use_simulated_output(
            [target](const std::vector<float>& block) {
                std::lock_guard<std::mutex> lock(target->mutex);
                if (target->first_call < 0) target->first_call = uptime_seconds();
                target->samples.insert(target->samples.end(), block.begin(), block.end());
            },
            stall_after);
    }

    /// 把一張 96×72 的畫面直接當成來源目前的那一格。
    void set_still(const Picture& picture) {
        auto frame = std::make_shared<SourceFrame>();
        frame->width = kImageWidth;
        frame->height = kImageHeight;
        frame->rgb.assign(picture.rgb.begin(), picture.rgb.end());
        app->debug_set_source_frame(frame);
    }

    bool wait(double timeout, const std::function<bool()>& condition) { return demo::wait_until(app.get(), timeout, condition); }
    bool wait_idle(double timeout) {
        return wait(timeout, [this] { return app->tx_phase == App::TxPhase::idle; }) && wait(3, [this] { return ptt->keys().size() % 2 == 0; });
    }
};

/// 把一段音訊交給接收機，回傳解出來的每一幀（後面補半秒靜音，讓最後一幀解完）。
std::vector<DecodedFrame> decode(std::vector<float> audio) {
    const std::vector<float> pad = demo::silence(0.5);
    audio.insert(audio.end(), pad.begin(), pad.end());
    std::vector<DecodedFrame> frames;
    for (const RxUpdate& update : demo::receive(audio)) {
        for (const DecodedFrame& frame : update.frames) frames.push_back(frame);
    }
    return frames;
}

Picture picture_or_gray(const char* name) {
    if (const std::optional<Picture> picture = demo::test_image(name)) return *picture;
    Picture gray;
    gray.rgb.fill(128);
    return gray;
}

// ---------------------------------------------------------------- 各項測試

void test_i18n(Check& c) {
    const std::string original = i18n::language();
    c.expect(i18n::languages().size() == 5, "語言應該有五種");
    for (const std::string& language : i18n::languages()) {
        i18n::use(language);
        c.expect(!L("設定").empty() && !L("開始發射").empty(), language + "：句子是空的");
        c.expect(contains(LF("已同步 %@", {"BX4ACP"}), "BX4ACP"), language + "：%@ 沒有填進去");
        c.expect(contains(LF("來源 %d×%d → 送出 96×72", {1920, 1080}), "1920") && contains(LF("來源 %d×%d → 送出 96×72", {1920, 1080}), "1080"), language + "：%d 沒有填進去");
        c.expect(contains(LF("%d 張", {3}), "3"), language + "：數量沒有填進去");
        if (language != "zh-Hant") c.expect(L("設定") != "設定" || language == "ja", language + "：「設定」沒有翻譯");
    }
    i18n::use("en");
    c.expect(L("設定") == "Settings", "英文的「設定」應該是 Settings，得到 " + L("設定"));
    c.expect(LF("%d 張", {1}) != LF("%d 張", {2}) && !contains(LF("%d 張", {1}), "frames"), "英文的單數：「" + LF("%d 張", {1}) + "」");
    i18n::use("zh-Hant");
    c.expect(L("設定") == "設定", "繁體中文就是原文");
    c.expect(i18n::language_for({"zh-TW"}) == "zh-Hant" && i18n::language_for({"zh_Hant_HK"}) == "zh-Hant", "zh-TW 應該挑到繁體中文");
    c.expect(i18n::language_for({"zh-CN"}) == "zh-Hans" && i18n::language_for({"zh_Hans_SG"}) == "zh-Hans", "zh-CN 應該挑到簡體中文");
    c.expect(i18n::language_for({"ja-JP"}) == "ja" && i18n::language_for({"es-419"}) == "es" && i18n::language_for({"en-GB"}) == "en", "ja、es、en");
    c.expect(i18n::language_for({"fr-FR"}) == "en" && i18n::language_for({}) == "en", "沒有翻譯的語言用英文");
    c.expect(i18n::language_for({"fr-FR", "ja-JP", "en-US"}) == "ja", "清單裡排最前面、有翻譯的那一個");
    c.info("系統的語言偏好挑到：" + i18n::system_language());
    i18n::use(original);
}

void test_settings(Check& c, const std::string& work) {
    const std::string path = work + "/settings 設定.txt";
    SDL_RemovePath(path.c_str());
    Settings fresh = Settings::load(path);
    c.expect(fresh.callsign.empty() && fresh.source_kind == SourceKind::video && fresh.ptt_mode == PTTMode::none && fresh.tx_mode_id == modes::default_id(), "沒有設定檔時的預設值");
    fresh.callsign = "BX4ACP";
    fresh.input_device = "USB Audio CODEC";
    fresh.output_device = "喇叭 (USB Audio)";
    fresh.output_level_db = -12;
    fresh.ptt_mode = PTTMode::cat;
    fresh.serial_port = "COM7";
    fresh.serial_line = SerialLine::dtr;
    fresh.rig_host = "radio.local";
    fresh.rig_port = "4533";
    fresh.rig_model = 3073;
    fresh.rig_model_name = "Icom IC-7300";
    fresh.serial_baud = 19200;
    fresh.tx_mode_id = 13;
    fresh.source_kind = SourceKind::camera;
    fresh.scale_mode = ScaleMode::fit;
    fresh.camera_name = "HD Webcam";
    fresh.loop_video = false;
    fresh.video_path = work + "/影片 video.mp4";
    fresh.schedule_interval_minutes = 30;
    fresh.schedule_skip_when_busy = false;
    fresh.show_frame_snr = true;
    fresh.canvas_enabled = true;
    fresh.canvas_text = "CQ CQ de BX4ACP";
    fresh.canvas_position = CanvasPosition::bottom;
    fresh.language = "ja";
    fresh.window_width = 1400;
    fresh.window_height = 900;
    c.expect(!fresh.loop_images && fresh.image_paths.empty() && !fresh.stamp_video_snr, "圖片序列與「存影片時印上訊雜比」的預設值");
    fresh.loop_images = true;
    fresh.image_paths = {work + "/相片 photos/IMG_0002.jpg", work + "/相片 photos/IMG_0010.jpg", "C:\\Users\\me\\Pictures\\a=b.png"};
    fresh.stamp_video_snr = true;
    fresh.save();
    std::uint64_t size = 0;
    c.expect(file_size(path, &size) && size > 100, "設定檔沒有寫出來：" + path);
    const Settings back = Settings::load(path);
    c.expect(back.callsign == "BX4ACP" && back.input_device == "USB Audio CODEC" && back.output_device == "喇叭 (USB Audio)", "呼號、裝置");
    c.expect(back.output_level_db == -12 && back.ptt_mode == PTTMode::cat && back.serial_port == "COM7" && back.serial_line == SerialLine::dtr, "音量、PTT、序列埠");
    c.expect(back.rig_host == "radio.local" && back.rig_port == "4533" && back.rig_model == 3073 && back.rig_model_name == "Icom IC-7300" && back.serial_baud == 19200, "電台");
    c.expect(back.tx_mode_id == 13 && back.source_kind == SourceKind::camera && back.scale_mode == ScaleMode::fit && back.camera_name == "HD Webcam", "模式、來源");
    c.expect(!back.loop_video && back.video_path == work + "/影片 video.mp4" && back.schedule_interval_minutes == 30 && !back.schedule_skip_when_busy, "影片、定時");
    c.expect(back.show_frame_snr && back.canvas_enabled && back.canvas_text == "CQ CQ de BX4ACP" && back.canvas_position == CanvasPosition::bottom, "顯示、畫布");
    c.expect(back.language == "ja" && back.window_width == 1400 && back.window_height == 900, "語言、視窗大小");
    c.expect(back.loop_images && back.stamp_video_snr && back.image_paths == fresh.image_paths, "圖片序列的循環播放與那一批路徑、存影片時印上訊雜比");
    Settings pictures = back;
    pictures.source_kind = SourceKind::images;
    if (!pictures.image_paths.empty()) pictures.image_paths.pop_back();
    pictures.save();
    const Settings fewer = Settings::load(path);
    c.expect(fewer.source_kind == SourceKind::images && fewer.image_paths.size() == 2 && fewer.image_paths == pictures.image_paths, "來源是圖片序列；那一批少了一張之後，檔案裡不能留著舊的那一行");
    c.expect(fewer.loop_images && fewer.stamp_video_snr, "沒有動到的設定要留著");
    c.expect(back.ptt_summary() != fresh.callsign && !back.ptt_summary().empty() && back.rig_model_title() == "Icom IC-7300", "PTT 的說明");
    // 不合理的值改回預設
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fputs("callsign=bx4acp/p!!\noutputLevelDb=99\nserialBaud=12345\ntxModeID=999\nscheduleIntervalMinutes=7\nlanguage=xx\n", file);
        std::fclose(file);
    }
    const Settings fixed = Settings::load(path);
    c.expect(fixed.callsign == "BX4ACP/P", "呼號要整理成大寫、去掉不能用的字元，得到「" + fixed.callsign + "」");
    c.expect(fixed.output_level_db == 0 && fixed.serial_baud == 0 && fixed.tx_mode_id == modes::default_id() && fixed.schedule_interval_minutes == schedule::kDefaultInterval && fixed.language.empty(),
             "不合理的值要改回預設");
}

void test_schedule(Check& c) {
    const double t = snapshot::time_today(14, 32, 7);
    c.expect(schedule::next_fire(t, 15) == snapshot::time_today(14, 45, 0), "14:32:07 之後每 15 分鐘的下一次是 14:45");
    c.expect(schedule::next_fire(snapshot::time_today(14, 45, 0), 15) == snapshot::time_today(15, 0, 0), "剛好在整點上：下一次是再下一個");
    c.expect(schedule::next_fire(t, 60) == snapshot::time_today(15, 0, 0) && schedule::next_fire(t, 5) == snapshot::time_today(14, 35, 0), "每 60、每 5 分鐘");
    c.expect(schedule::clock_text(snapshot::time_today(9, 5, 0)) == "09:05" && schedule::time_text(t) == "14:32:07", "時刻的文字");
    c.expect(contains(schedule::minutes_text(15), "00") && contains(schedule::minutes_text(15), "45") && contains(schedule::minutes_text(5), "55"), "每小時的哪幾分：" + schedule::minutes_text(15));
    // 開關與下一次的時刻（不發射）
    Settings settings = Settings::load("");
    App app(settings, false);
    app.clock = [t] { return t; };
    c.expect(!app.schedule_armed && app.schedule_summary() == L("定時發射"), "一開始是關的");
    app.set_schedule_armed(true);
    c.expect(app.schedule_armed && app.schedule_next_fire && *app.schedule_next_fire == snapshot::time_today(14, 45, 0), "打開後下一次是 14:45");
    c.expect(app.schedule_problem().has_value(), "沒有呼號、沒有影片時要說得出原因");
    app.settings.schedule_interval_minutes = 5;
    app.schedule_interval_changed();
    c.expect(app.schedule_next_fire && *app.schedule_next_fire == snapshot::time_today(14, 35, 0), "改成每 5 分鐘：下一次是 14:35");
    app.set_schedule_armed(false);
    c.expect(!app.schedule_next_fire, "關掉後沒有下一次");
}

void test_models(Check& c) {
    std::string ids;
    for (const ModeInfo& mode : modes::all()) ids += std::to_string(mode.id) + " ";
    c.expect(modes::all().size() == 4, "應該有四個模式，實際：" + ids);
    c.expect(modes::find(11) != nullptr && modes::find(12) != nullptr && modes::find(13) != nullptr && modes::find(14) != nullptr, "模式 11、12、13、14");
    c.expect(modes::default_id() == 11, "預設是模式 11");
    std::string order;
    for (const ModeInfo& mode : modes::menu_order()) order += std::to_string(mode.id) + " ";
    c.expect(order == "14 12 11 13 ", "選單的順序應該是 14 12 11 13，得到 " + order);
    for (const ModeInfo& mode : modes::all()) {
        c.expect(mode.frame_seconds > 0.3 && mode.frame_seconds < 10 && !mode.model_name.empty() && !mode.label().empty() && !mode.summary().empty(), "模式 " + std::to_string(mode.id) + " 的資料不完整");
    }
    c.expect(normalize_callsign(" bx4acp/p ") == "BX4ACP/P" && normalize_callsign("abcdefghijkl").size() == 9, "呼號的整理");
}

void test_loopback(Check& c) {
    const Picture coffee = picture_or_gray("coffee");
    for (const ModeInfo& mode : modes::menu_order()) {
        CoreTransmitter tx;
        c.expect(tx.set_mode(mode.id), "模式 " + std::to_string(mode.id) + " 設定不了");
        tx.set_callsign("BX4ACP");
        const std::optional<EncodedFrame> first = tx.encode(coffee), second = tx.encode(coffee);
        if (!first || !second) {
            c.expect(false, "模式 " + std::to_string(mode.id) + " 編碼失敗");
            continue;
        }
        c.expect(std::fabs(static_cast<double>(first->samples.size()) / kSampleRate - mode.frame_seconds) < 0.05, "模式 " + std::to_string(mode.id) + " 一幀的長度不對");
        std::vector<float> audio = demo::silence(0.4);
        audio.insert(audio.end(), first->samples.begin(), first->samples.end());
        audio.insert(audio.end(), second->samples.begin(), second->samples.end());
        const std::vector<float> tail = tx.finish();
        audio.insert(audio.end(), tail.begin(), tail.end());
        const std::vector<DecodedFrame> frames = decode(audio);
        c.expect(frames.size() == 2, "模式 " + std::to_string(mode.id) + "：送兩幀應該收到兩幀，收到 " + std::to_string(frames.size()));
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const DecodedFrame& frame = frames[i];
            c.expect(frame.decoded && frame.picture != nullptr && frame.header_ok && frame.callsign == "BX4ACP" && frame.mode_id == mode.id && frame.sequence == static_cast<int>(i),
                     "模式 " + std::to_string(mode.id) + " 第 " + std::to_string(i + 1) + " 幀的內容不對");
            if (frame.picture) {
                const double same = demo::psnr(*frame.picture, *first->preview), like = demo::psnr(*frame.picture, coffee);
                c.expect(same > 30, "模式 " + std::to_string(mode.id) + "：沒有雜訊時收到的應該和發射端的預覽幾乎一樣，PSNR " + number(same, 1));
                if (i == 0) c.info("模式 " + std::to_string(mode.id) + "（" + number(mode.frame_seconds) + " 秒）：和預覽 " + number(same, 1) + " dB，和原圖 " + number(like, 1) + " dB");
            }
        }
    }
}

void test_channel(Check& c) {
    const Picture rocket = picture_or_gray("rocket");
    CoreTransmitter tx;
    tx.set_mode(11);
    tx.set_callsign("N0CALL");
    const std::optional<EncodedFrame> frame = tx.encode(rocket);
    if (!frame) return c.expect(false, "編碼失敗");
    std::vector<float> signal = frame->samples;
    const std::vector<float> tail = tx.finish();
    signal.insert(signal.end(), tail.begin(), tail.end());
    std::vector<float> audio = demo::silence(0.6);
    audio.insert(audio.end(), signal.begin(), signal.end());
    const std::vector<float> pad = demo::silence(0.6);
    audio.insert(audio.end(), pad.begin(), pad.end());
    // 訊雜比 14 dB、電台偏了 +12 Hz、輸入音量約 −18 dB
    const std::vector<float> received = demo::radio_channel(audio, demo::power(signal), 14.0, 12.0, -32, -18.0);
    const std::vector<DecodedFrame> frames = decode(received);
    c.expect(frames.size() == 1 && frames[0].decoded && frames[0].callsign == "N0CALL", "有雜訊、有頻偏時應該解得出一幀，解出 " + std::to_string(frames.size()));
    if (frames.size() == 1) {
        c.expect(std::fabs(frames[0].snr_db - 14) < 3 && std::fabs(frames[0].offset_hz - 12) < 3, "量到的訊雜比 " + number(frames[0].snr_db, 1) + " dB、頻偏 " + number(frames[0].offset_hz, 1) + " Hz");
        c.info("訊雜比 " + number(frames[0].snr_db, 1) + " dB，頻偏 " + number(frames[0].offset_hz, 1) + " Hz");
    }
    // 只有雜訊：不能誤解出畫面
    const std::vector<float> noise = demo::radio_channel(demo::silence(6), 0, std::nullopt, 0, -32, std::nullopt, 7);
    c.expect(decode(noise).empty(), "只有雜訊時不應該解出任何一幀");
}

void test_scale(Check& c) {
    // 2×：每 2×2 個像素平均成一個
    SourceFrame doubled;
    doubled.width = kImageWidth * 2;
    doubled.height = kImageHeight * 2;
    doubled.rgb.resize(static_cast<std::size_t>(doubled.width) * doubled.height * 3);
    const Picture cat = picture_or_gray("cat");
    for (int y = 0; y < doubled.height; ++y) {
        for (int x = 0; x < doubled.width; ++x) {
            for (int k = 0; k < 3; ++k) doubled.rgb[(static_cast<std::size_t>(y) * doubled.width + x) * 3 + k] = cat.rgb[(static_cast<std::size_t>(y / 2) * kImageWidth + x / 2) * 3 + k];
        }
    }
    c.expect(demo::psnr(scale_frame(doubled, ScaleMode::fill), cat) > 45 && demo::psnr(scale_frame(doubled, ScaleMode::fit), cat) > 45, "放大兩倍的圖縮回來應該和原圖一樣");
    // 16:9 的白色畫面：填滿時整張都是白的，完整時上下補黑
    SourceFrame wide;
    wide.width = 320;
    wide.height = 180;
    wide.rgb.assign(static_cast<std::size_t>(wide.width) * wide.height * 3, 255);
    const Picture filled = scale_frame(wide, ScaleMode::fill), fitted = scale_frame(wide, ScaleMode::fit);
    const auto pixel = [](const Picture& picture, int x, int y) { return picture.rgb[(static_cast<std::size_t>(y) * kImageWidth + x) * 3]; };
    c.expect(pixel(filled, 48, 0) == 255 && pixel(filled, 48, 71) == 255 && pixel(filled, 0, 36) == 255, "填滿：16:9 的畫面要蓋滿整張");
    c.expect(pixel(fitted, 48, 0) == 0 && pixel(fitted, 48, 71) == 0 && pixel(fitted, 48, 36) == 255 && pixel(fitted, 0, 36) == 255, "完整：16:9 的畫面上下補黑、左右到邊");
    // 直的畫面：完整時左右補黑
    SourceFrame tall;
    tall.width = 180;
    tall.height = 320;
    tall.rgb.assign(static_cast<std::size_t>(tall.width) * tall.height * 3, 255);
    const Picture tall_fit = scale_frame(tall, ScaleMode::fit);
    c.expect(pixel(tall_fit, 0, 36) == 0 && pixel(tall_fit, 95, 36) == 0 && pixel(tall_fit, 48, 0) == 255 && pixel(tall_fit, 48, 71) == 255, "完整：直的畫面左右補黑、上下到邊");
}

void test_png(Check& c, const std::string& work) {
    const Picture coffee = picture_or_gray("coffee");
    const std::string path = work + "/咖啡 coffee.png";
    c.expect(write_png(path, coffee.rgb.data(), kImageWidth, kImageHeight, 3), "PNG 寫不出來");
    const SourceFramePtr loaded = load_image_file(path);
    c.expect(loaded != nullptr && loaded->width == kImageWidth && loaded->height == kImageHeight, "PNG 讀不回來");
    if (loaded && loaded->rgb.size() == coffee.rgb.size()) c.expect(std::equal(coffee.rgb.begin(), coffee.rgb.end(), loaded->rgb.begin()), "PNG 讀回來的像素和寫進去的不一樣");
    const std::vector<std::uint8_t> bytes = encode_png(coffee.rgb.data(), kImageWidth, kImageHeight, 3);
    c.expect(bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G', "PNG 的檔頭");
    c.expect(load_image_file(work + "/沒有這個檔.png") == nullptr, "不存在的檔案要回傳空的");
}

void test_fonts(Check& c) {
    for (const std::string& language : i18n::languages()) {
        const FontSet fonts = find_fonts(language);
        const auto first = [](const std::vector<FontFile>& list) { return list.empty() ? std::string("（沒有）") : list.front().path; };
        c.info(language + "：一般 " + first(fonts.regular) + (fonts.regular.size() > 1 ? "＋" + fonts.regular[1].path : "") + "，等寬 " + first(fonts.mono) + "，中日文 " + (fonts.has_cjk ? "有" : "沒有"));
        if (fonts.regular.empty()) c.skip("找不到任何字型檔（介面會用 Dear ImGui 內附的英文字型）", "fonts");
        else if (!fonts.has_cjk) c.skip("找不到中文、日文的字型（介面會改用英文顯示）", "cjk");
        for (const FontFile& file : fonts.regular) c.expect(file_size(file.path, nullptr), "字型檔不存在：" + file.path);
    }
}

void test_canvas(Check& c) {
    CanvasLayer::set_fonts(find_fonts(i18n::language()).bold);
    if (!CanvasLayer::has_fonts()) return c.skip("找不到字型，畫布印不了字", "fonts");
    Picture gray;
    gray.rgb.fill(120);
    const std::shared_ptr<const CanvasLayer> top = CanvasLayer::make(CanvasStyle{"CQ BX4ACP", CanvasPosition::top});
    c.expect(top != nullptr, "畫布的圖層做不出來");
    if (!top) return;
    c.expect(top->lines().size() == 1 && top->lines()[0] == "CQ BX4ACP", "短的字排成一行");
    c.expect(top->first_row() == 0 && top->last_row() > 8 && top->last_row() < 36 && top->font_size() >= CanvasLayer::kWrapBelowFontSize, "字帶在上緣，高度合理");
    const Picture stamped = top->apply(gray);
    bool outside_untouched = true, inside_changed = false;
    for (int y = 0; y < kImageHeight; ++y) {
        for (int x = 0; x < kImageWidth * 3; ++x) {
            const bool changed = stamped.rgb[static_cast<std::size_t>(y) * kImageWidth * 3 + x] != 120;
            if (y >= top->first_row() && y < top->last_row()) inside_changed = inside_changed || changed;
            else if (changed) outside_untouched = false;
        }
    }
    c.expect(inside_changed && outside_untouched, "字帶裡面要有字，字帶外面一個位元組都不能動");
    const std::shared_ptr<const CanvasLayer> bottom = CanvasLayer::make(CanvasStyle{"CQ BX4ACP", CanvasPosition::bottom});
    c.expect(bottom && bottom->last_row() == kImageHeight && bottom->first_row() > 36, "印在下緣");
    const std::shared_ptr<const CanvasLayer> wrapped = CanvasLayer::make(CanvasStyle{"CQ CQ DE BX4ACP K", CanvasPosition::top});
    c.expect(wrapped && wrapped->lines().size() == 2 && wrapped->font_size() <= CanvasLayer::kWrappedMaximumFontSize && wrapped->last_row() > top->last_row(), "一行放不下的字拆成兩行");
    const std::shared_ptr<const CanvasLayer> cut = CanvasLayer::make(CanvasStyle{"CQCQCQDEBX4ACPBX4ACPPSEKPSEK", CanvasPosition::top});
    c.expect(cut && cut->lines().size() == 1 && cut->lines()[0].size() > 3 && cut->lines()[0].compare(cut->lines()[0].size() - 3, 3, "…") == 0 && cut->font_size() == CanvasLayer::kMinimumFontSize,
             "用最小的字還放不下：截短，結尾是「…」");
    c.expect(CanvasLayer::make(CanvasStyle{"   ", CanvasPosition::top}) == nullptr, "沒有字就沒有圖層");
    c.info("「CQ BX4ACP」字級 " + number(top->font_size(), 1) + "，字帶第 " + std::to_string(top->first_row()) + "–" + std::to_string(top->last_row() - 1) + " 列");
}

void test_waterfall(Check& c) {
    WaterfallStore store;
    c.expect(store.pixels().size() == static_cast<std::size_t>(WaterfallStore::kWidth) * WaterfallStore::kHeight, "瀑布圖的大小");
    const std::uint64_t before = store.version();
    const std::vector<float> noise = demo::radio_channel(demo::silence(2), 0, std::nullopt, 0, -32, std::nullopt, 3);
    const auto rows = demo::monitor_rows(noise);
    c.expect(rows.size() > 30, "兩秒的聲音應該有幾十列頻譜，得到 " + std::to_string(rows.size()));
    for (const auto& row : rows) store.append(row, WaterfallStore::Reference::noise);
    c.expect(store.version() >= before + rows.size() / WaterfallStore::kRowsPerLine - 1, "每湊滿兩列畫一列");
    // 通帶裡（1500 Hz）要比通帶外（100 Hz）亮
    const auto brightness = [&store](double hz) {
        const std::uint32_t pixel = store.pixels()[static_cast<std::size_t>(hz / kSpectrumMaxHz * (WaterfallStore::kWidth - 1))];
        const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&pixel);
        return static_cast<int>(bytes[0]) + bytes[1] + bytes[2];
    };
    c.expect(brightness(1500) > brightness(100), "通帶裡的雜訊要比通帶外亮");
}

void test_audio_output(Check& c) {
    AudioOutput output(2.0);
    output.simulated = true;
    std::mutex mutex;
    std::vector<float> got;
    output.simulated_sink = [&](const std::vector<float>& block) {
        std::lock_guard<std::mutex> lock(mutex);
        got.insert(got.end(), block.begin(), block.end());
    };
    output.gain = 0.5f;
    std::string error;
    c.expect(output.start(0, &error) && output.running(), "模擬的輸出開不起來");
    std::vector<float> ramp(14400);
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i + 1) / 14400.0f;
    c.expect(output.enqueue(ramp), "排不進佇列");
    c.expect(!output.enqueue(std::vector<float>(3 * kSampleRate, 0.0f)), "超過佇列長度的要拒絕");
    // 接著排一段、馬上拿掉：不應該播出來
    const std::int64_t mark = static_cast<std::int64_t>(ramp.size());
    c.expect(output.enqueue(std::vector<float>(9600, 0.9f)) && output.discard_queued(mark) && output.queued_samples() <= mark, "排進去還沒播的要拿得掉");
    c.expect(demo::wait_until(nullptr, 3, [&] { return output.played_samples() >= mark && output.silent_frames() > 2000; }), "取樣沒有照時間被取走");
    c.expect(!output.discard_queued(100), "已經播過的位置不能再拿掉");
    output.stop();
    c.expect(!output.running(), "停不下來");
    std::lock_guard<std::mutex> lock(mutex);
    bool exact = got.size() >= ramp.size();
    for (std::size_t i = 0; exact && i < ramp.size(); ++i) exact = std::fabs(got[i] - 0.5f * ramp[i]) < 1e-6f;
    bool silent_after = true;
    for (std::size_t i = ramp.size(); i < got.size(); ++i) silent_after = silent_after && got[i] == 0.0f;
    c.expect(exact, "取走的取樣要和排進去的一樣（乘上音量）");
    c.expect(silent_after, "佇列空了之後給的是靜音，拿掉的那一段不能出現");
    c.expect(output.underruns() == 0 || output.underruns() == 1, "佇列取空的次數");

    // 裝置卡住：過了 0.2 秒就不再來取
    AudioOutput stalled(2.0);
    stalled.simulated = true;
    stalled.simulated_stall_after = 0.2;
    stalled.start(0, &error);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    const std::int64_t first = stalled.rendered_frames();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    c.expect(first > 0 && first < 14000 && stalled.rendered_frames() == first, "卡住之後取的格數不再增加（" + std::to_string(first) + "）");
    stalled.stop();
}

void test_net(Check& c) {
    {
        net::LineServer server([](const std::string& line) { return line == "T 1" ? std::string("RPRT 0\n") : std::string("RPRT -1\n"); });
        c.expect(server.port() > 0, "測試用的伺服器開不起來");
        const net::Reply ok = net::request("127.0.0.1", std::to_string(server.port()), "T 1\n", 2);
        c.expect(ok.ok() && ok.text == "RPRT 0\n", "送一行、收一行");
        const net::Reply other = net::request("localhost", std::to_string(server.port()), "X\n", 2);
        c.expect(other.ok() && other.text == "RPRT -1\n", "用主機名稱 localhost 連線");
        const std::vector<std::string> lines = server.received();
        c.expect(lines.size() == 2 && lines[0] == "T 1" && lines[1] == "X", "伺服器收到的指令");
        c.expect(net::can_connect_local(server.port()), "連得上");
    }
    const int free_port = net::free_local_port();
    c.expect(free_port > 0 && !net::can_connect_local(free_port), "沒有人聽的連接埠連不上");
    const net::Reply refused = net::request("127.0.0.1", std::to_string(free_port), "T 1\n", 1);
    c.expect(refused.failure == net::Reply::Failure::connect, "連不上要回報「連不上」");
    net::LineServer mute([](const std::string&) { return std::string(); });
    const net::Reply dropped = net::request("127.0.0.1", std::to_string(mute.port()), "T 1\n", 2);
    c.expect(dropped.failure == net::Reply::Failure::no_reply && !dropped.timed_out, "對方不回覆就掛斷：沒有回覆，但不是逾時");
}

void test_rigctld(Check& c) {
    {
        net::LineServer server([](const std::string&) { return std::string("RPRT 0\n"); });
        RigctldPTT ptt("127.0.0.1", std::to_string(server.port()));
        c.expect(!ptt.key(true) && !ptt.key(false), "rigctld 回覆 RPRT 0 就是成功");
        const std::vector<std::string> lines = server.received();
        c.expect(lines.size() == 2 && lines[0] == "T 1" && lines[1] == "T 0", "按下送「T 1」，放開送「T 0」");
    }
    {
        net::LineServer server([](const std::string&) { return std::string("RPRT -9\n"); });
        RigctldPTT ptt("127.0.0.1", std::to_string(server.port()));
        const PTTResult error = ptt.key(true);
        c.expect(error.has_value() && contains(error->message, "RPRT -9"), "rigctld 回報錯誤時要失敗，並帶著它的回覆");
    }
    RigctldPTT nobody("127.0.0.1", std::to_string(net::free_local_port()), 1);
    const PTTResult error = nobody.key(true);
    c.expect(error.has_value() && !error->timed_out && !error->may_be_keyed, "連不上 rigctld：失敗，指令沒有送到所以不可能按著");
    // 設定裡的四種 PTT 各做得出驅動
    Settings settings = Settings::load("");
    for (const PTTMode mode : {PTTMode::none, PTTMode::serial, PTTMode::cat, PTTMode::rigctld}) {
        settings.ptt_mode = mode;
        c.expect(settings.make_ptt_driver() != nullptr && !settings.ptt_summary().empty(), std::string("PTT 驅動：") + to_string(mode));
        c.expect(ptt_mode_from(to_string(mode)) == mode, "PTT 模式的名稱要轉得回來");
    }
    settings.ptt_mode = PTTMode::serial;
    settings.serial_port.clear();
    const PTTResult no_port = settings.make_ptt_driver()->key(true);
    c.expect(no_port.has_value(), "沒有選序列埠時按 PTT 要失敗（不能默默當成成功）");
}

void test_hamlib_parse(Check& c) {
    std::string listing = " Rig #  Mfg                    Model                   Version         Status      Macro\n";
    const auto row = [&listing](int id, const char* mfg, const char* model, const char* macro) {
        char line[200];
        std::snprintf(line, sizeof line, "%6d  %-23s%-24s%-16s%-12s%s\n", id, mfg, model, "20240101.0", "Stable", macro);
        listing += line;
    };
    row(1, "Hamlib", "Dummy", "RIG_MODEL_DUMMY");
    row(2, "Hamlib", "NET rigctl", "RIG_MODEL_NETRIGCTL");
    row(6, "Hamlib", "Dummy No VFO", "RIG_MODEL_DUMMY_NOVFO");
    row(3073, "Icom", "IC-7300", "RIG_MODEL_IC7300");
    row(3085, "Icom", "IC-705", "RIG_MODEL_IC705");
    row(1035, "Yaesu", "FT-991", "RIG_MODEL_FT991");
    const std::vector<hamlib::Model> models = hamlib::parse_models(listing);
    c.expect(models.size() == 6, "清單應該有 6 個型號，得到 " + std::to_string(models.size()));
    for (const hamlib::Model& model : models) {
        if (model.id == 2) c.expect(model.manufacturer == "Hamlib" && model.name == "NET rigctl", "名稱裡有空白的型號：「" + model.name + "」");
        if (model.id == 6) c.expect(model.name == "Dummy No VFO", "名稱裡有兩個空白的型號：「" + model.name + "」");
        if (model.id == 3073) c.expect(model.title() == "Icom IC-7300", "廠牌加型號：「" + model.title() + "」");
    }
    c.expect(hamlib::parse_models("rigctl: command not found\n").empty(), "不是清單的文字解不出型號");
    const std::optional<hamlib::SerialCaps> plain = hamlib::parse_serial_caps("Model name:\tIC-7300\nSerial speed: 4800..115200 baud, 8N1, ctrl=NONE\n");
    c.expect(plain && plain->min_baud == 4800 && plain->max_baud == 115200 && !plain->hardware_handshake, "序列埠規格");
    const std::optional<hamlib::SerialCaps> hard = hamlib::parse_serial_caps("Serial speed: 4800..38400 baud, 8N2, ctrl=Hardware\n");
    c.expect(hard && hard->hardware_handshake, "要用 RTS／CTS 流量控制的型號");
    c.expect(!hamlib::parse_serial_caps("Model name:\tDummy\n"), "不走序列埠的型號沒有規格");
    const std::vector<std::string> args = HamlibPTT::arguments(3073, "/dev/ttyUSB0", 19200, 4533, plain);
    const std::vector<std::string> wanted = {"-m", "3073", "-T", "127.0.0.1", "-t", "4533", "-r", "/dev/ttyUSB0", "-s", "19200", "-C", "rts_state=OFF,dtr_state=OFF"};
    c.expect(args == wanted, "rigctld 的參數");
    const std::vector<std::string> hard_args = HamlibPTT::arguments(1035, "COM5", 0, 4533, hard);
    c.expect(hard_args.back() == "dtr_state=OFF" && std::find(hard_args.begin(), hard_args.end(), "-s") == hard_args.end(), "硬體流量控制的型號只管 DTR；沒有指定速率就不帶 -s");
    c.expect(HamlibPTT::arguments(1, "", 0, 4533, std::nullopt).size() == 6, "不走序列埠的型號不帶序列埠的參數");
    const std::optional<std::string> reason = HamlibPTT::open_failure_reason("rig_open: error = IO error\nserial_open: Unable to open /dev/ttyUSB9 - No such file or directory\nrig_open returning(-6) IO error\n");
    c.expect(reason && contains(*reason, "Unable to open /dev/ttyUSB9"), "開不了電台的原因");
    c.expect(!HamlibPTT::open_failure_reason("Opened rig model 1, 'Dummy'\n"), "沒有出錯的訊息不算");
    c.expect(hamlib::baud_rates().size() >= 6, "可以選的速率");
}

void test_process(Check& c, const Options& options) {
    if (options.executable.empty()) return c.expect(false, "查不出這個執行檔自己的路徑");
    const std::optional<std::string> version = run_and_capture({options.executable, "--version"}, 20);
    c.expect(version && contains(*version, "nbtv-gui"), "另外啟動一個自己、讀它印的字：「" + version.value_or("（沒有）") + "」");
    c.expect(find_program("no-such-program-nbtv-selftest").empty(), "不存在的程式找不到");
    // 管線：握著它的標準輸入，放掉時它就該動作（看門的程序就是靠這個）。假的 rigctld 會收到「T 0」，另外那個行程會被結束
    net::LineServer server([](const std::string&) { return std::string("RPRT 0\n"); });
    Process::Options victim_options;
    victim_options.args = {options.executable, "--sleep", "60"};
    std::string error;
    std::unique_ptr<Process> victim = Process::spawn(victim_options, &error);
    if (!victim) return c.expect(false, "啟動不了另一個自己：" + error);
    Process::Options dog_options;
    dog_options.args = {options.executable, "--ptt-watchdog", std::to_string(server.port()), std::to_string(victim->pid())};
    dog_options.pipe_stdin = true;
    std::unique_ptr<Process> dog = Process::spawn(dog_options, &error);
    if (!dog) return c.expect(false, "啟動不了看門的程序：" + error);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    c.expect(!dog->exited() && !victim->exited() && server.received().empty(), "管線還握著的時候看門的程序什麼都不做");
    dog->close_stdin();
    int code = -1;
    c.expect(dog->wait(8, &code) && code == 0, "管線放掉後看門的程序要做完事結束");
    const std::vector<std::string> lines = server.received();
    c.expect(!lines.empty() && lines[0] == "T 0", "看門的程序要對 rigctld 送「T 0」");
    c.expect(victim->wait(5), "看門的程序要把那個行程結束掉");
    // 等不到的行程：時間到了回傳 false，然後強制結束
    Process::Options sleeper_options;
    sleeper_options.args = {options.executable, "--sleep", "60"};
    std::unique_ptr<Process> sleeper = Process::spawn(sleeper_options, &error);
    if (sleeper) {
        c.expect(!sleeper->wait(0.3) && !sleeper->exited(), "還在跑的行程等不到");
        sleeper->kill(true);
        c.expect(sleeper->wait(5), "強制結束");
    }
}

void test_hamlib_ptt(Check& c) {
    if (!hamlib::installed()) return c.skip("這台電腦沒有裝 Hamlib（rigctld、rigctl）", "hamlib");
    const std::vector<hamlib::Model> models = hamlib::models();
    c.expect(models.size() > 100, "「rigctl -l」應該列出幾百個型號，得到 " + std::to_string(models.size()));
    c.expect(!hamlib::models_by_manufacturer().empty(), "依廠牌分組");
    // 型號 1 是 Hamlib 內建的假電台，不碰任何硬體；rigctld 只聽本機的一個臨時連接埠
    {
        HamlibPTT ptt(1, "", 0);
        const PTTResult pressed = ptt.key(true);
        c.expect(!pressed, "用假電台按下 PTT：" + (pressed ? pressed->message : std::string()));
        const std::optional<int> port = ptt.tcp_port();
        c.expect(port.has_value(), "按下之後 rigctld 應該在執行");
        if (port) {
            std::string reply;
            RigctldPTT query("127.0.0.1", std::to_string(*port));
            c.expect(!query.send("t\n", reply) && reply.rfind("1", 0) == 0, "問 rigctld：PTT 應該是按著的，它回「" + reply + "」");
            c.expect(!ptt.key(false), "放開 PTT");
            c.expect(!ptt.tcp_port().has_value() && demo::wait_until(nullptr, 3, [&] { return !net::can_connect_local(*port); }), "放開之後 rigctld 要結束");
        }
    }
    // app 突然不在了：看門的程序要替我們放開 PTT、結束 rigctld
    {
        HamlibPTT ptt(1, "", 0);
        c.expect(!ptt.key(true), "再按一次");
        const std::optional<int> port = ptt.tcp_port();
        if (port) {
            ptt.debug_abandon();
            c.expect(demo::wait_until(nullptr, 8, [&] { return !net::can_connect_local(*port); }), "app 不在之後，看門的程序要把 rigctld 結束掉");
        }
    }
    c.info("Hamlib 的型號 " + std::to_string(models.size()) + " 個");
}

void test_serial(Check& c) {
    const std::vector<std::string> ports = serial_ports();   // 只列出來，不開啟任何一個
    c.info("序列埠 " + std::to_string(ports.size()) + " 個");
    SerialPort port;
    std::string error;
    c.expect(!port.open("/definitely/not/a/serial/port", &error) && !port.is_open() && !error.empty(), "不存在的序列埠開不起來，而且有原因");
}

void test_transmit(Check& c) {
    const Picture coffee = picture_or_gray("coffee");
    Rig rig(12);
    c.expect(!rig.app->can_transmit(), "還沒有來源畫面時不能發射");
    rig.set_still(coffee);
    c.expect(rig.app->can_transmit(), "有來源畫面就可以發射");
    rig.app->start_transmit();
    c.expect(rig.app->tx_phase == App::TxPhase::starting, "按下發射後先是「準備發射」");
    c.expect(rig.wait(40, [&] { return rig.app->tx_frames_sent >= 2; }), "30 秒內應該送出兩張");
    c.expect(rig.app->is_on_air() && rig.app->tx_preview != nullptr && rig.app->tx_elapsed > 1.5, "發射中：有預覽、有已發射的時間");
    rig.app->stop_transmit();
    c.expect(rig.app->tx_phase == App::TxPhase::stopping, "按停止後先送完目前這張");
    c.expect(rig.wait_idle(20), "送完之後要回到待機");
    const int sent = rig.app->tx_frames_sent;
    const std::vector<bool> keys = rig.ptt->keys();
    c.expect(keys == std::vector<bool>({true, false}), "PTT 要剛好按下一次、放開一次（實際 " + std::to_string(keys.size()) + " 次動作）");
    c.expect(!rig.app->notice.has_value() && !rig.app->ptt_warning.has_value(), "正常結束不應該有訊息：" + rig.app->notice.value_or(""));
    const std::vector<float> audio = rig.sink->take();
    std::size_t first_sound = audio.size();
    for (std::size_t i = 0; i < audio.size(); ++i) {
        if (audio[i] != 0.0f) {
            first_sound = i;
            break;
        }
    }
    c.expect(first_sound >= 14000 && first_sound < 16000, "開頭要有 0.3 秒的靜音（第一個有聲音的取樣在 " + std::to_string(first_sound) + "）");
    {
        std::lock_guard<std::mutex> lock(rig.ptt->mutex);
        c.expect(!rig.ptt->events.empty() && rig.ptt->events[0].second <= rig.sink->first_call, "要先按下 PTT 才開始送聲音");
    }
    const std::vector<DecodedFrame> frames = decode(audio);
    // 按停止時正在送的那一張會送完；它是第二張還是第三張看機器多快
    c.expect(static_cast<int>(frames.size()) == sent && sent >= 2, "送出 " + std::to_string(sent) + " 張，從送出的聲音解出 " + std::to_string(frames.size()) + " 張");
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const DecodedFrame& frame = frames[i];
        c.expect(frame.decoded && frame.header_ok && frame.callsign == "BX4ACP" && frame.mode_id == 12 && frame.sequence == static_cast<int>(i), "第 " + std::to_string(i + 1) + " 張的標頭不對");
        if (frame.picture) c.expect(demo::psnr(*frame.picture, coffee) > 15, "第 " + std::to_string(i + 1) + " 張的畫面不像送出去的那一張（PSNR " + number(demo::psnr(*frame.picture, coffee), 1) + "）");
    }
    c.info("送出 " + std::to_string(sent) + " 張，共 " + number(static_cast<double>(audio.size()) / kSampleRate, 1) + " 秒的聲音");
    // 沒有呼號不能發射
    Rig nameless(12);
    nameless.app->settings.callsign.clear();
    nameless.set_still(coffee);
    nameless.app->start_transmit();
    c.expect(nameless.app->tx_phase == App::TxPhase::idle && nameless.app->notice.has_value() && nameless.ptt->keys().empty(), "沒有呼號時不能發射，PTT 不能動");
}

void test_transmit_canvas(Check& c) {
    CanvasLayer::set_fonts(find_fonts(i18n::language()).bold);
    if (!CanvasLayer::has_fonts()) return c.skip("找不到字型，畫布印不了字", "fonts");
    const Picture cat = picture_or_gray("cat");
    Rig rig(11);
    rig.app->settings.canvas_text = "CQ TEST";
    rig.app->set_canvas_enabled(true);
    rig.set_still(cat);
    c.expect(rig.wait(5, [&] { return rig.app->source_preview != nullptr; }), "預覽沒有出來");
    const Picture stamped = rig.app->apply_canvas(cat);
    c.expect(demo::psnr(stamped, cat) < 40, "畫布打開時送出的畫面上要有字");
    if (rig.app->source_preview) c.expect(demo::psnr(*rig.app->source_preview, stamped) > 45, "預覽就是印好字、會送出去的樣子");
    rig.app->start_transmit();
    c.expect(rig.wait(40, [&] { return rig.app->tx_frames_sent >= 1; }), "應該送出一張");
    rig.app->stop_transmit();
    c.expect(rig.wait_idle(20), "要回到待機");
    const std::vector<DecodedFrame> frames = decode(rig.sink->take());
    c.expect(!frames.empty() && frames[0].picture != nullptr, "解不出送出去的畫面");
    if (!frames.empty() && frames[0].picture) {
        const double with_text = demo::psnr(*frames[0].picture, stamped), without = demo::psnr(*frames[0].picture, cat);
        c.expect(with_text > without, "收到的畫面應該比較像印了字的那一張（" + number(with_text, 1) + " 對 " + number(without, 1) + " dB）");
    }
    rig.app->set_canvas_enabled(false);
    c.expect(demo::psnr(rig.app->apply_canvas(cat), cat) > 90, "畫布關掉後畫面原樣送出");
}

void test_transmit_abort(Check& c) {
    Rig rig(11);
    rig.set_still(picture_or_gray("rocket"));
    rig.app->start_transmit();
    c.expect(rig.wait(40, [&] { return rig.app->tx_phase == App::TxPhase::transmitting && rig.app->tx_elapsed > 0.2; }), "應該開始送了");
    rig.app->toggle_transmit();
    c.expect(rig.app->tx_phase == App::TxPhase::stopping, "按一次：送完這張就停");
    rig.app->toggle_transmit();
    c.expect(rig.app->tx_phase == App::TxPhase::stopping, "馬上再按一次不算（防止連按兩下）");
    demo::spin(rig.app.get(), App::kAbortGuardSeconds + 0.1);
    c.expect(rig.app->tx_phase == App::TxPhase::stopping, "模式 11 一張 2.72 秒，這時候應該還沒送完");
    rig.app->toggle_transmit();
    c.expect(rig.app->tx_phase == App::TxPhase::idle, "過了半秒再按一次：立刻停止");
    c.expect(rig.wait_idle(5) && rig.ptt->keys() == std::vector<bool>({true, false}), "立刻停止也要放開 PTT");
    const std::vector<DecodedFrame> cut_short = decode(rig.sink->take());
    c.expect(cut_short.empty(), "沒送完的那一張對方解不出來（解出 " + std::to_string(cut_short.size()) + " 張）");
    // 還沒開始送就取消
    Rig early(13);
    early.set_still(picture_or_gray("rocket"));
    early.app->start_transmit();
    early.app->stop_transmit();
    c.expect(early.app->tx_phase == App::TxPhase::idle && early.wait_idle(10), "還在準備時按停止：直接取消");
    demo::spin(early.app.get(), 1.0);
    const std::vector<bool> keys = early.ptt->keys();
    c.expect(keys.empty() || keys == std::vector<bool>({true, false}), "取消之後 PTT 不能留在按著");
    c.expect(early.sink->take().empty(), "取消之後不應該送出任何聲音");
}

void test_transmit_failures(Check& c) {
    // PTT 按不下去，而且說不準電台有沒有按下
    {
        Rig rig(12, std::nullopt, true, true);
        rig.set_still(picture_or_gray("cat"));
        rig.app->start_transmit();
        c.expect(rig.wait(40, [&] { return rig.app->tx_phase == App::TxPhase::idle && rig.app->notice.has_value(); }), "PTT 失敗後要回到待機並說明原因");
        c.expect(rig.wait(3, [&] { return rig.ptt->keys().size() == 2; }) && rig.ptt->keys() == std::vector<bool>({true, false}), "按不成功也要放開一次");
        c.expect(rig.app->ptt_warning.has_value(), "說不準電台有沒有按下時，要在主視窗上警告");
        c.expect(rig.sink->take().empty(), "PTT 失敗就不能送出聲音");
    }
    // 輸出裝置送到一半不回應了
    {
        const double original = App::output_stall_seconds;
        App::output_stall_seconds = 0.6;
        Rig rig(11, 0.5);
        rig.set_still(picture_or_gray("cat"));
        rig.app->start_transmit();
        c.expect(rig.wait(40, [&] { return rig.app->tx_phase == App::TxPhase::transmitting; }), "應該開始送了");
        c.expect(rig.wait(10, [&] { return rig.app->tx_phase == App::TxPhase::idle; }), "輸出裝置不回應時要自己停下來");
        c.expect(rig.wait(3, [&] { return rig.ptt->keys().size() == 2; }) && rig.ptt->keys() == std::vector<bool>({true, false}), "輸出裝置不回應時要放開 PTT");
        c.expect(rig.app->notice.has_value(), "要說明為什麼停了");
        App::output_stall_seconds = original;
    }
}

void test_file_sources(Check& c, const std::string& work) {
    const Picture rocket = picture_or_gray("rocket"), cat = picture_or_gray("cat");
    // 圖片：自己讀，不需要 ffmpeg
    {
        const std::string path = work + "/火箭 rocket.png";
        c.expect(demo::write_picture_png(rocket, path, 4), "測試用的圖片寫不出來");
        Rig rig(12);
        std::optional<bool> opened;
        rig.app->open_video(path, true, std::nullopt, [&opened](bool ok) { opened = ok; });
        c.expect(rig.wait(10, [&] { return opened.has_value(); }) && opened.value_or(false), "圖片開不起來：" + rig.app->source_message.value_or(""));
        c.expect(rig.app->video_is_image && rig.app->video_name == std::optional<std::string>("火箭 rocket.png") && rig.app->settings.video_path == path, "圖片的名稱、記下來的路徑");
        c.expect(rig.wait(5, [&] { return rig.app->source_preview != nullptr && rig.app->can_transmit(); }), "圖片的預覽沒有出來");
        c.expect(rig.app->source_size == std::optional<std::pair<int, int>>(std::make_pair(kImageWidth * 4, kImageHeight * 4)), "圖片的大小");
        if (rig.app->source_preview) c.expect(demo::psnr(*rig.app->source_preview, rocket) > 45, "圖片縮成 96×72 應該和原圖一樣");
        c.expect(!rig.app->video_plays_once(), "圖片沒有「播完」");
        // 開不起來的檔案
        std::optional<bool> bad;
        rig.app->open_video(work + "/不存在.mp4", false, std::nullopt, [&bad](bool ok) { bad = ok; });
        c.expect(rig.wait(15, [&] { return bad.has_value(); }) && !bad.value_or(true) && rig.app->source_message.has_value(), "不存在的檔案要說明開不起來");
    }
    if (!FileSource::ffmpeg_available()) return c.skip("這台電腦沒有 ffmpeg，影片檔的部分沒有測", "ffmpeg");
    const std::string clip = work + "/貓 clip.mp4";
    // 16:9 的影片：4:3 的圖放大後上下多出來的部分用來移動，每一格都不一樣
    if (!demo::make_video(clip, cat, 3, 5, 320, 180)) return c.expect(false, "測試用的影片做不出來（ffmpeg 在 " + FileSource::ffmpeg_path() + "）");
    Rig rig(12);
    rig.app->settings.loop_video = false;
    std::optional<bool> opened;
    rig.app->open_video(clip, false, std::nullopt, [&opened](bool ok) { opened = ok; });
    c.expect(rig.wait(30, [&] { return opened.has_value(); }) && opened.value_or(false), "影片開不起來：" + rig.app->source_message.value_or(""));
    if (!opened.value_or(false)) return;
    c.expect(!rig.app->video_is_image && rig.app->video_duration > 2.4 && rig.app->video_duration < 3.7, "影片的長度應該是 3 秒，得到 " + number(rig.app->video_duration));
    c.expect(rig.wait(5, [&] { return rig.app->source_preview != nullptr && rig.app->can_transmit(); }), "影片開頭那一格的預覽沒有出來");
    c.expect(rig.app->source_size == std::optional<std::pair<int, int>>(std::make_pair(320, 180)),
             "影片原本的大小應該是 320×180，得到 " + (rig.app->source_size ? std::to_string(rig.app->source_size->first) + "×" + std::to_string(rig.app->source_size->second) : std::string("—")));
    c.expect(!rig.app->video_playing && rig.app->video_plays_once(), "沒有勾循環播放：開了先不播，按下發射才開始");
    // 移動位置
    rig.app->scrub_video(1.5, false);
    c.expect(rig.wait(10, [&] { return std::fabs(rig.app->video_position - 1.5) < 0.3; }), "移到 1.5 秒");
    // 發射：影片從目前的位置播到完，自動停止
    rig.app->start_transmit(true);   // 從頭
    c.expect(rig.wait(40, [&] { return rig.app->is_on_air(); }), "應該開始送了");
    c.expect(rig.wait(5, [&] { return rig.app->video_playing; }), "開始送的同時影片要開始播");
    c.expect(rig.wait_idle(40), "影片播完要自動停止發射");
    c.expect(rig.app->tx_frames_sent >= 3, "3 秒的影片用 1.04 秒一張的模式，至少送 3 張，實際 " + std::to_string(rig.app->tx_frames_sent));
    c.expect(rig.ptt->keys() == std::vector<bool>({true, false}) && !rig.app->notice.has_value(), "播完自動停止是正常結束：" + rig.app->notice.value_or(""));
    c.expect(!rig.app->video_playing, "停止後影片也停著");
    const std::vector<DecodedFrame> frames = decode(rig.sink->take());
    c.expect(static_cast<int>(frames.size()) == rig.app->tx_frames_sent, "送出的每一張都要解得出來：" + std::to_string(frames.size()) + "／" + std::to_string(rig.app->tx_frames_sent));
    if (frames.size() >= 2 && frames.front().picture && frames.back().picture) {
        c.expect(demo::psnr(*frames.front().picture, *frames.back().picture) < 40, "影片在動：頭尾兩張不應該一樣");
    }
    c.info("3 秒的影片送出 " + std::to_string(rig.app->tx_frames_sent) + " 張");
    // 循環播放：開了就播，播完從頭再來
    Rig looping(12);
    std::optional<bool> again;
    looping.app->open_video(clip, false, std::nullopt, [&again](bool ok) { again = ok; });
    c.expect(looping.wait(30, [&] { return again.has_value(); }) && again.value_or(false), "影片再開一次");
    c.expect(looping.wait(5, [&] { return looping.app->video_playing; }), "勾了循環播放：開了就播");
    looping.app->toggle_video_playback();
    c.expect(!looping.app->video_playing, "暫停");
    looping.app->shutdown();
}

void test_history(Check& c, const std::string& work) {
    App app(Settings::load(""), false);
    const Picture coffee = picture_or_gray("coffee");
    const double base = snapshot::time_today(10, 0, 0);
    const auto add = [&](double time, const char* call, int mode = 11) {
        DecodedFrame frame;
        frame.mode_id = mode;
        frame.decoded = true;
        frame.header_ok = call[0] != '\0';
        frame.callsign = call;
        frame.sequence = 0;
        frame.snr_db = 12.5;
        frame.picture = std::make_shared<Picture>(coffee);
        app.add(frame, time);
    };
    const double frame = modes::find(11) != nullptr ? modes::find(11)->frame_seconds : 2.72;
    // 第一段：N0CALL 連續三張；隔 30 秒；第二段：BX4ACP 兩張緊接著 JA1XYZ 一張
    add(base, "N0CALL");
    add(base + frame, "N0CALL");
    add(base + 2 * frame, "");   // 標頭沒解出來：跟著前面那一段
    add(base + 2 * frame + 30, "BX4ACP");
    add(base + 3 * frame + 30, "BX4ACP");
    add(base + 4 * frame + 30, "JA1XYZ");
    c.expect(app.history.size() == 6 && app.displayed_frame() == &app.history.front(), "新的在前，主畫面是最新的那一張");
    const auto size_of = [&](std::size_t index) { return App::sequence(app.history[index].id, app.history).size(); };
    c.expect(size_of(5) == 3 && size_of(4) == 3 && size_of(3) == 3, "第一段三張（含標頭沒解出來的那張）");
    c.expect(size_of(2) == 2 && size_of(1) == 2 && size_of(0) == 1, "呼號不同就是另一段");
    const std::vector<App::ReceivedFrame> first = App::sequence(app.history[4].id, app.history);
    c.expect(first.size() == 3 && first.front().time < first.back().time, "一段裡由舊到新");
    app.select(app.history[4]);
    c.expect(app.is_displayed(app.history[4]) && app.displayed_sequence().size() == 3, "點選縮圖：主畫面換成那一張");
    add(base + 5 * frame + 30, "JA1XYZ");
    c.expect(app.displayed_frame() == &app.history.front(), "有新的畫面進來，主畫面換成它");
    c.expect(contains(App::export_file_name(app.history.front()), "NBTV-JA1XYZ-") && contains(App::video_file_name(first), "NBTV-N0CALL-"), "存檔的檔名");
    // 存圖片
    const std::string png = work + "/收到 received.png";
    app.select(app.history[1]);
    app.save_displayed_frame(png);
    const SourceFramePtr saved = load_image_file(png);
    c.expect(saved && saved->width == kImageWidth && saved->height == kImageHeight && std::equal(coffee.rgb.begin(), coffee.rgb.end(), saved->rgb.begin()), "存出來的 PNG 要和收到的畫面一模一樣");
    // 解碼失敗、模型不符：不進清單，只改「上一張」
    DecodedFrame failed;
    failed.mode_id = 11;
    app.add(failed, base + 100);
    c.expect(app.history.size() == 7 && app.last_result == App::LastResult::failed, "解碼失敗");
    failed.model_missing = true;
    app.add(failed, base + 101);
    c.expect(app.last_result == App::LastResult::model_missing, "模型不符");
    // 存影片
    if (!FileSource::ffmpeg_available()) return c.skip("這台電腦沒有 ffmpeg，存影片的部分沒有測", "ffmpeg");
    const std::string movie = work + "/收到 received.mp4";
    std::optional<bool> done;
    app.export_sequence(first, movie, [&done](bool ok) { done = ok; });
    c.expect(app.exporting_video, "存影片的時候要標示「儲存中」");
    c.expect(demo::wait_until(&app, 60, [&] { return done.has_value(); }) && done.value_or(false), "影片存不出來：" + app.notice.value_or(""));
    std::uint64_t size = 0;
    c.expect(file_size(movie, &size) && size > 1000 && !app.exporting_video, "存出來的影片檔");
    FileSource source;
    const std::optional<std::string> error = source.open(movie);
    const double expected = 2 * frame + std::max(1.0, frame);
    c.expect(!error && std::fabs(source.duration() - expected) < 0.6, "三張的影片應該長 " + number(expected) + " 秒，得到 " + number(source.duration()) + (error ? "（" + *error + "）" : ""));
    const SourceFramePtr still = source.current_frame();
    c.expect(still && still->shown_width() == 960 && still->shown_height() == 720, "影片的大小是 960×720");
}

// ---------------------------------------------------------------- 存成影片時印上訊雜比

/// 兩塊同樣大小（寬 width）的 RGB 在 (x, y) 起 w×h 那個範圍裡平均每個值差多少。
double region_difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int width, int x, int y, int w, int h) {
    if (a.size() != b.size() || width <= 0 || a.size() % (static_cast<std::size_t>(width) * 3) != 0) return 255;
    const int height = static_cast<int>(a.size() / (static_cast<std::size_t>(width) * 3));
    const int x0 = std::max(0, x), x1 = std::min(width, x + w), y0 = std::max(0, y), y1 = std::min(height, y + h);
    if (x1 <= x0 || y1 <= y0) return 255;
    long long total = 0;
    for (int row = y0; row < y1; ++row) {
        for (std::size_t i = (static_cast<std::size_t>(row) * width + x0) * 3; i < (static_cast<std::size_t>(row) * width + x1) * 3; ++i) total += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return static_cast<double>(total) / (static_cast<double>(x1 - x0) * (y1 - y0) * 3);
}

/// 一張 width×height 的 RGB 左上角 w×h 那一塊。
std::vector<std::uint8_t> top_left(const std::vector<std::uint8_t>& rgb, int width, int w, int h) {
    std::vector<std::uint8_t> block(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) std::copy_n(rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * width * 3), w * 3, block.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * w * 3));
    return block;
}

/// 用 ffmpeg 把影片在第 seconds 秒顯示的那一格解出來，經過 filter 之後是 width×height 的 RGB。讀不回來是空的。
std::vector<std::uint8_t> video_frame_at(const std::string& path, double seconds, const std::string& filter, int width, int height) {
    const std::optional<std::string> raw = run_and_capture({FileSource::ffmpeg_path(), "-nostdin", "-hide_banner", "-loglevel", "error", "-ss", number(seconds, 4), "-i", path,
                                                           "-frames:v", "1", "-vf", filter, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"}, 60);
    if (!raw || raw->size() != static_cast<std::size_t>(width) * height * 3) return {};
    return std::vector<std::uint8_t>(raw->begin(), raw->end());
}

/// 存成影片時「在每一張的左上角印上訊雜比」：
///   * 要印的字是那一張自己的訊雜比（「SNR 12.3 dB」，負的用數學的減號）；沒勾的時候時間軸裡沒有字
///   * 那一塊在左上角、不大；畫出來是半透明的黑底加白字，那一塊以外一個位元組都不動
///   * 影片實際寫出來再讀回：勾了，每一張的左上角是它自己的訊雜比（和預期的樣子比，而且比「印上別張的訊雜比」像得多），
///     那一塊以外的畫面不受影響；沒勾，左上角就是原來的畫面
///   * 找不到字型時影片照存，並說明沒有印
void test_video_stamp(Check& c, const std::string& work) {
    const std::vector<Picture> pictures = {picture_or_gray("coffee"), picture_or_gray("rocket"), picture_or_gray("cat")};
    // 三張各隔一秒收到，訊雜比都不一樣（有一張是負的）
    const double levels[3] = {12.3, -4.7, 28.0};
    const double base = snapshot::time_today(10, 0, 0);
    std::vector<App::ReceivedFrame> frames;
    for (int i = 0; i < 3; ++i) {
        App::ReceivedFrame frame;
        frame.id = static_cast<std::uint64_t>(i + 1);
        frame.picture = std::make_shared<Picture>(pictures[static_cast<std::size_t>(i)]);
        frame.mode_id = 12;
        frame.callsign = "N0CALL";
        frame.sequence = i;
        frame.time = base + i;
        frame.snr_db = levels[i];
        frames.push_back(frame);
    }
    std::vector<std::string> labels;
    for (const App::VideoPicture& picture : App::video_timeline(frames, true).pictures) labels.push_back(picture.label);
    c.expect(labels == std::vector<std::string>({"SNR 12.3 dB", "SNR −4.7 dB", "SNR 28.0 dB"}), "要印的字是 " + (labels.size() == 3 ? labels[0] + "、" + labels[1] + "、" + labels[2] : std::string("（張數不對）")));
    bool any_label = false;
    for (const App::VideoPicture& picture : App::video_timeline(frames).pictures) any_label = any_label || !picture.label.empty();
    const App::VideoTimeline timeline = App::video_timeline(frames);
    c.expect(!any_label && timeline.pictures.size() == 3 && timeline.pictures[2].start == 2.0 && timeline.last_hold >= 1.0, "沒有要印的時候時間軸裡不能有字；每一張從它收到的時刻開始");
    c.expect(!Settings::load("").stamp_video_snr, "預設不印訊雜比");
    if (labels.size() != 3) return;

    // 找不到字型：印不了，畫面原樣
    Picture gray;
    gray.rgb.fill(120);
    const std::vector<std::uint8_t> plain_gray = sequence_video::frame_image(gray, "");
    VideoStamp::set_fonts({});
    c.expect(!VideoStamp::has_fonts() && VideoStamp::rect_for(labels[0]).width == 0 && sequence_video::frame_image(gray, labels[0]) == plain_gray, "沒有字型時不能動到畫面");
    VideoStamp::set_fonts(find_fonts(i18n::language()).mono_bold);
    if (!VideoStamp::has_fonts()) return c.skip("找不到字型，訊雜比印不出來", "fonts");

    // ---- 那一塊的位置與樣子
    const int corner_w = 400, corner_h = 100;
    std::vector<VideoStamp::Rect> rects;
    for (const std::string& label : labels) {
        const VideoStamp::Rect rect = VideoStamp::rect_for(label);
        rects.push_back(rect);
        c.expect(rect.x >= 4 && rect.y >= 4 && rect.x + rect.width <= corner_w && rect.y + rect.height <= corner_h && rect.width * rect.height <= 0.04 * sequence_video::kWidth * sequence_video::kHeight,
                 "「" + label + "」那一塊的位置是 " + std::to_string(rect.x) + "," + std::to_string(rect.y) + " 起 " + std::to_string(rect.width) + "×" + std::to_string(rect.height));
    }
    c.expect(plain_gray.size() == static_cast<std::size_t>(sequence_video::kWidth) * sequence_video::kHeight * 3 && sequence_video::kWidth == 960 && sequence_video::kHeight == 720, "影片的一格是 960×720");
    {
        // 沒有字的那一格就是 96×72 的畫面每個像素放大成 10×10
        const std::vector<std::uint8_t> plain = sequence_video::frame_image(pictures[0], "");
        bool exact = true;
        for (int y = 0; y < sequence_video::kHeight && exact; y += 7) {
            for (int x = 0; x < sequence_video::kWidth && exact; x += 3) {
                exact = std::equal(plain.begin() + (static_cast<std::ptrdiff_t>(y) * sequence_video::kWidth + x) * 3, plain.begin() + (static_cast<std::ptrdiff_t>(y) * sequence_video::kWidth + x) * 3 + 3,
                                   pictures[0].rgb.begin() + (static_cast<std::ptrdiff_t>(y / 10) * kImageWidth + x / 10) * 3);
            }
        }
        c.expect(exact, "沒有字的那一格應該就是每個像素放大成 10×10");
    }
    {
        // 灰色的畫面上印字：方塊裡沒有字的地方是壓暗的灰（120 的 38%），字是白的，圓角的最外面那一點不動，方塊以外一個位元組都不動
        const std::vector<std::uint8_t> stamped = sequence_video::frame_image(gray, labels[0]);
        const VideoStamp::Rect rect = rects[0];
        const auto at = [&stamped](int x, int y) { return static_cast<int>(stamped[(static_cast<std::size_t>(y) * sequence_video::kWidth + static_cast<std::size_t>(x)) * 3 + 1]); };
        bool outside_untouched = true;
        int brightest = 0;
        for (int y = 0; y < sequence_video::kHeight; ++y) {
            for (int x = 0; x < sequence_video::kWidth; ++x) {
                const bool inside = x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
                if (inside) brightest = std::max(brightest, at(x, y));
                else if (at(x, y) != 120) outside_untouched = false;
            }
        }
        const int padding = at(rect.x + 4, rect.y + rect.height / 2);
        c.expect(outside_untouched, "那一塊以外的畫面被動到了");
        c.expect(std::abs(padding - 46) <= 1 && brightest >= 250 && at(rect.x, rect.y) == 120 && at(rect.x + rect.width - 1, rect.y + rect.height - 1) == 120,
                 "那一塊的樣子不對：黑底的地方是 " + std::to_string(padding) + "（預期 46），最亮 " + std::to_string(brightest) + "，角落 " + std::to_string(at(rect.x, rect.y)));
        c.expect(sequence_video::frame_image(gray, labels[2]) != stamped && sequence_video::frame_image(gray, labels[0]) == stamped, "不同的字印出來要不一樣，同樣的字每次都一樣");
        c.info("「" + labels[0] + "」那一塊在 " + std::to_string(rect.x) + "," + std::to_string(rect.y) + "，大小 " + std::to_string(rect.width) + "×" + std::to_string(rect.height));
    }

    // ---- 影片實際寫出來再讀回
    if (!FileSource::ffmpeg_available()) return c.skip("這台電腦沒有 ffmpeg，寫成影片檔的部分沒有測", "ffmpeg");
    App app(Settings::load(""), false);
    /// 照設定存一次影片；回傳存成了沒有
    const auto save = [&](bool stamp, const std::string& path) {
        app.settings.stamp_video_snr = stamp;
        app.notice.reset();
        std::optional<bool> done;
        app.export_sequence(frames, path, [&done](bool ok) { done = ok; });
        return demo::wait_until(&app, 60, [&] { return done.has_value(); }) && done.value_or(false);
    };
    const std::string stamped_path = work + "/印訊雜比 stamped.mp4", plain_path = work + "/不印 plain.mp4";
    if (!save(true, stamped_path) || !save(false, plain_path)) return c.expect(false, "影片存不出來：" + app.notice.value_or(""));
    c.expect(app.notice && contains(*app.notice, "不印 plain.mp4") && contains(*app.notice, "3"), "存完要在底列寫出檔名與張數：" + app.notice.value_or(""));
    {
        // 勾不勾，影片的大小與長度都一樣
        FileSource stamped_file, plain_file;
        const std::optional<std::string> error_a = stamped_file.open(stamped_path), error_b = plain_file.open(plain_path);
        const SourceFramePtr still = stamped_file.current_frame();
        c.expect(!error_a && !error_b && still && still->shown_width() == 960 && still->shown_height() == 720, "印了訊雜比的影片也是 960×720");
        c.expect(std::fabs(stamped_file.duration() - plain_file.duration()) < 0.05 && std::fabs(stamped_file.duration() - 3.04) < 0.3,
                 "三張各隔一秒的影片應該長 3 秒左右，得到 " + number(stamped_file.duration()) + " 與 " + number(plain_file.duration()));
    }
    std::string details;
    for (int index = 0; index < 3; ++index) {
        const double when = index + 0.5;   // 這一張顯示到一半的時候
        const std::string corner_filter = "crop=" + std::to_string(corner_w) + ":" + std::to_string(corner_h) + ":0:0", small_filter = "scale=" + std::to_string(kImageWidth) + ":" + std::to_string(kImageHeight) + ":flags=neighbor";
        const std::vector<std::uint8_t> stamped_corner = video_frame_at(stamped_path, when, corner_filter, corner_w, corner_h), plain_corner = video_frame_at(plain_path, when, corner_filter, corner_w, corner_h);
        const std::vector<std::uint8_t> stamped_small = video_frame_at(stamped_path, when, small_filter, kImageWidth, kImageHeight), plain_small = video_frame_at(plain_path, when, small_filter, kImageWidth, kImageHeight);
        if (stamped_corner.empty() || plain_corner.empty() || stamped_small.empty() || plain_small.empty()) {
            c.expect(false, "第 " + std::to_string(index + 1) + " 張的那一格讀不回來");
            continue;
        }
        const Picture& picture = pictures[static_cast<std::size_t>(index)];
        const auto expected = [&](const std::string& label) { return top_left(sequence_video::frame_image(picture, label), sequence_video::kWidth, corner_w, corner_h); };
        const std::vector<std::uint8_t> own = expected(labels[static_cast<std::size_t>(index)]), bare = expected("");
        const VideoStamp::Rect rect = rects[static_cast<std::size_t>(index)];
        const double match = region_difference(stamped_corner, own, corner_w, rect.x, rect.y, rect.width, rect.height);
        const double unstamped = region_difference(stamped_corner, bare, corner_w, rect.x, rect.y, rect.width, rect.height);
        // 印上別張的訊雜比會是什麼樣子：兩塊字佔的範圍合起來比
        double confused = 255;
        for (int other = 0; other < 3; ++other) {
            if (other == index) continue;
            const VideoStamp::Rect& theirs = rects[static_cast<std::size_t>(other)];
            const int x = std::min(rect.x, theirs.x), y = std::min(rect.y, theirs.y);
            const int w = std::max(rect.x + rect.width, theirs.x + theirs.width) - x, h = std::max(rect.y + rect.height, theirs.y + theirs.height) - y;
            const std::vector<std::uint8_t> wrong = expected(labels[static_cast<std::size_t>(other)]);
            confused = std::min(confused, region_difference(stamped_corner, wrong, corner_w, x, y, w, h) - region_difference(stamped_corner, own, corner_w, x, y, w, h));
        }
        c.expect(match <= 12 && unstamped >= 20 && confused >= 5,
                 "第 " + std::to_string(index + 1) + " 張左上角不是「" + labels[static_cast<std::size_t>(index)] + "」（和預期的平均差 " + number(match, 1) + "、和沒印字的差 " + number(unstamped, 1) + "、印成別張的話多差 " + number(confused, 1) + "）");
        // 沒勾的那一份：左上角是原來的畫面
        const double clean = region_difference(plain_corner, bare, corner_w, rect.x, rect.y, rect.width, rect.height);
        c.expect(clean <= 12, "沒有勾的時候第 " + std::to_string(index + 1) + " 張的左上角也被改了（和原來的畫面平均差 " + number(clean, 1) + "）");
        // 那一塊以外：和原圖一樣，勾不勾沒有差別（那一塊在 96×72 的畫面裡不超過最上面 8 列）
        const std::vector<std::uint8_t> original(picture.rgb.begin(), picture.rgb.end());
        const double rest = region_difference(stamped_small, original, kImageWidth, 0, 8, kImageWidth, kImageHeight - 8);
        const double same = region_difference(stamped_small, plain_small, kImageWidth, 0, 8, kImageWidth, kImageHeight - 8);
        c.expect(rest <= 8 && same <= 3, "第 " + std::to_string(index + 1) + " 張印字以外的地方變了（和原圖平均差 " + number(rest, 1) + "、和沒勾的差 " + number(same, 1) + "）");
        details += (details.empty() ? "" : "；") + std::string("第 ") + std::to_string(index + 1) + " 張「" + labels[static_cast<std::size_t>(index)] + "」和預期的平均差 " + number(match, 1) + "／255（沒印字的話 " + number(unstamped, 0) + "）";
    }
    if (!details.empty()) c.info(details);

    // 找不到字型時：影片照存，底列說明沒有印
    VideoStamp::set_fonts({});
    const std::string fontless = work + "/沒有字型 nofont.mp4";
    c.expect(save(true, fontless) && file_size(fontless, nullptr), "找不到字型時影片也要存得出來");
    c.expect(app.notice == std::optional<std::string>(LF("影片已存成「%@」，但找不到字型，沒有印上訊雜比", {"沒有字型 nofont.mp4"})), "找不到字型時要說明沒有印：" + app.notice.value_or(""));
    VideoStamp::set_fonts(find_fonts(i18n::language()).mono_bold);
}

void test_schedule_run(Check& c, const std::string& work) {
    const std::string path = work + "/定時 still.png";
    if (!demo::write_picture_png(picture_or_gray("coffee"), path, 2)) return c.expect(false, "測試用的圖片寫不出來");
    const double original = App::still_schedule_seconds;
    App::still_schedule_seconds = 1.4;
    Rig rig(12);
    std::optional<bool> opened;
    rig.app->open_video(path, false, std::nullopt, [&opened](bool ok) { opened = ok; });
    c.expect(rig.wait(10, [&] { return opened.has_value(); }) && opened.value_or(false), "圖片開不起來");
    c.expect(rig.wait(5, [&] { return rig.app->can_transmit(); }), "來源畫面沒有出來");
    // 假的時鐘：現在是整點前 0.8 秒
    const double fire = schedule::next_fire(schedule::now() + 120, 5), began = uptime_seconds();
    rig.app->clock = [fire, began] { return fire - 0.8 + (uptime_seconds() - began); };
    rig.app->settings.schedule_interval_minutes = 5;
    rig.app->set_schedule_armed(true);
    c.expect(rig.app->schedule_next_fire && *rig.app->schedule_next_fire == fire && !rig.app->schedule_problem(), "下一次的時刻");
    c.expect(rig.wait(1, [&] { return rig.app->schedule_countdown.has_value(); }), "最後一分鐘裡要倒數");
    c.expect(rig.wait(5, [&] { return rig.app->scheduled_run; }), "到了時間要自己開始發射");
    c.expect(rig.wait(40, [&] { return rig.app->is_on_air(); }), "應該開始送了");
    c.expect(rig.wait_idle(40), "圖片送滿指定的時間後要自己停止");
    c.expect(!rig.app->scheduled_run && rig.app->schedule_last_event.has_value() && rig.app->tx_frames_sent >= 2, "要記下這一次的結果，送出 " + std::to_string(rig.app->tx_frames_sent) + " 張");
    c.expect(rig.ptt->keys() == std::vector<bool>({true, false}), "定時發射結束要放開 PTT");
    c.expect(rig.app->schedule_armed && rig.app->schedule_next_fire && *rig.app->schedule_next_fire > fire, "開關還開著，排好了下一次");
    if (rig.app->schedule_last_event) c.info("上一次：" + *rig.app->schedule_last_event);
    // 正在收別人的畫面：那一次不發射
    Rig busy(12);
    std::optional<bool> again;
    busy.app->open_video(path, false, std::nullopt, [&again](bool ok) { again = ok; });
    c.expect(busy.wait(10, [&] { return again.has_value(); }), "圖片再開一次");
    const double began2 = uptime_seconds();
    busy.app->clock = [fire, began2] { return fire - 0.5 + (uptime_seconds() - began2); };
    busy.app->settings.schedule_interval_minutes = 5;
    busy.app->set_schedule_armed(true);
    busy.app->synced = true;
    c.expect(busy.wait(5, [&] { return busy.app->schedule_last_event.has_value(); }), "到了時間要留下紀錄");
    c.expect(busy.app->tx_phase == App::TxPhase::idle && busy.ptt->keys().empty() && busy.app->notice.has_value(), "正在收別人的畫面時不發射，PTT 不動");
    busy.app->set_schedule_armed(false);
    rig.app->set_schedule_armed(false);
    // 來源是圖片序列時到了時間：和鏡頭一樣先換成影片檔，送完再換回圖片序列（那一批圖片與位置都留著），送出去的是影片檔那一邊的畫面
    {
        const std::string folder = work + "/定時 pictures";
        SDL_CreateDirectory(folder.c_str());
        const Picture rocket = picture_or_gray("rocket"), cat = picture_or_gray("cat"), coffee = picture_or_gray("coffee");
        demo::write_picture_png(rocket, folder + "/p1.png", 2);
        demo::write_picture_png(cat, folder + "/p2.png", 2);
        demo::write_picture_png(rocket, folder + "/p3.png", 2);
        Rig sequence(12);
        App& app = *sequence.app;
        app.set_source_kind(SourceKind::images);
        c.expect(app.open_images({folder}, false) == 3, "圖片序列選不起來");
        app.select_image(1);
        std::optional<bool> third;
        app.open_video(path, false, std::nullopt, [&third](bool ok) { third = ok; });
        c.expect(sequence.wait(10, [&] { return third.has_value(); }) && third.value_or(false), "圖片（定時發射要送的）開不起來");
        c.expect(app.settings.source_kind == SourceKind::images && sequence.wait(5, [&] { return app.can_transmit() && app.source_preview != nullptr; }) && demo::psnr(*app.source_preview, cat) > 45,
                 "開了定時發射要用的檔案之後，來源還是圖片序列，預覽是下一張要送的那一張");
        const double began3 = uptime_seconds();
        app.clock = [fire, began3] { return fire - 0.5 + (uptime_seconds() - began3); };
        app.settings.schedule_interval_minutes = 5;
        app.set_schedule_armed(true);
        c.expect(sequence.wait(5, [&] { return app.scheduled_run; }) && app.settings.source_kind == SourceKind::video, "到了時間：來源先換成影片檔");
        c.expect(sequence.wait_idle(40), "定時發射要自己停止");
        c.expect(app.settings.source_kind == SourceKind::images && app.image_files.size() == 3 && app.image_index == 1, "送完要換回圖片序列，那一批圖片與位置都留著（位置在第 " + std::to_string(app.image_index + 1) + " 張）");
        c.expect(sequence.wait(5, [&] { return app.can_transmit() && app.source_preview != nullptr && demo::psnr(*app.source_preview, cat) > 45; }), "換回圖片序列之後，預覽要再出來");
        const std::vector<DecodedFrame> frames = decode(sequence.sink->take());
        bool from_video = !frames.empty();
        for (const DecodedFrame& frame : frames) from_video = from_video && frame.picture && demo::psnr(*frame.picture, coffee) > std::max(demo::psnr(*frame.picture, cat), demo::psnr(*frame.picture, rocket));
        c.expect(from_video && app.tx_frames_sent >= 1, "定時發射送的應該是影片檔那一邊的畫面，不是圖片序列的（送出 " + std::to_string(app.tx_frames_sent) + " 張）");
        app.set_schedule_armed(false);
    }
    App::still_schedule_seconds = original;
}

// ---------------------------------------------------------------- 圖片序列

std::string file_part(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// 幾個檔名接成一句（出問題時說明用）
std::string listed(const std::vector<std::string>& paths) {
    std::string text;
    for (const std::string& path : paths) text += (text.empty() ? "" : "、") + file_part(path);
    return text.empty() ? "（沒有）" : text;
}

/// 把一段文字寫成檔案（做「不是圖片」「壞掉的圖片」用）。
void write_text(const std::string& path, const std::string& text) {
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}

/// 四個角各有一塊不同顏色的畫面（其餘是灰的）：左上紅、右上綠、左下藍、右下白。看一張圖被轉成什麼方向用。
const std::uint8_t kCornerColors[4][3] = {{230, 30, 30}, {30, 200, 30}, {40, 60, 230}, {240, 240, 240}};
Picture corner_marks() {
    Picture picture;
    picture.rgb.fill(128);
    const int block_w = kImageWidth / 4, block_h = kImageHeight / 4;
    for (int corner = 0; corner < 4; ++corner) {
        const int x0 = corner % 2 == 0 ? 0 : kImageWidth - block_w, y0 = corner < 2 ? 0 : kImageHeight - block_h;
        for (int y = y0; y < y0 + block_h; ++y) {
            for (int x = x0; x < x0 + block_w; ++x) std::copy_n(kCornerColors[corner], 3, picture.rgb.begin() + (static_cast<std::ptrdiff_t>(y) * kImageWidth + x) * 3);
        }
    }
    return picture;
}

/// 一張畫面的左上、右上、左下、右下各是 corner_marks 的哪一塊（0 紅、1 綠、2 藍、3 白）；認不出來是 -1。
std::array<int, 4> corner_colors(const SourceFrame& frame) {
    std::array<int, 4> found{};
    for (int corner = 0; corner < 4; ++corner) {
        // 每一塊佔寬高各四分之一：取它正中間的那一點
        const int x = corner % 2 == 0 ? frame.width / 8 : frame.width - 1 - frame.width / 8, y = corner < 2 ? frame.height / 8 : frame.height - 1 - frame.height / 8;
        const std::uint8_t* pixel = frame.rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)) * 3;
        found[static_cast<std::size_t>(corner)] = -1;
        for (int color = 0; color < 4; ++color) {
            int distance = 0;
            for (int k = 0; k < 3; ++k) distance += std::abs(static_cast<int>(pixel[k]) - kCornerColors[color][k]);
            if (distance < 90) found[static_cast<std::size_t>(corner)] = color;   // JPEG 會讓顏色差一點
        }
    }
    return found;
}

/// 八條直的彩條（96×72）：和內附的三張測試圖都不像，當第四張圖用。
Picture color_bars() {
    const std::uint8_t colors[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0}};
    Picture picture;
    for (int y = 0; y < kImageHeight; ++y) {
        for (int x = 0; x < kImageWidth; ++x) std::copy_n(colors[x * 8 / kImageWidth], 3, picture.rgb.begin() + (static_cast<std::ptrdiff_t>(y) * kImageWidth + x) * 3);
    }
    return picture;
}

/// 圖片序列的檔案：選到的資料夾換成它裡面的圖片、照檔名排（數字照大小、不分大小寫）、不是圖片的與隱藏的略過、
/// 不往子資料夾裡找、同一個檔案只算一次；讀圖時照相片記載的方向轉正，大圖縮到夠用的大小，壞掉的檔案讀不進來。
void test_image_files(Check& c, const std::string& work) {
    const Picture coffee = picture_or_gray("coffee");
    const std::string folder = work + "/圖片 images", inner = folder + "/sub";
    SDL_CreateDirectory(folder.c_str());
    SDL_CreateDirectory(inner.c_str());
    SDL_CreateDirectory((folder + "/folder.png").c_str());   // 名字像圖片的資料夾：不算
    for (const char* name : {"10.png", "2.png", "1.png", ".hidden.png", "sub/inner.png"}) c.expect(demo::write_picture_png(coffee, folder + "/" + name, 1), std::string("寫不出 ") + name);
    for (const char* name : {"b.JPG", "A.jpg"}) c.expect(demo::write_picture_jpeg(coffee, folder + "/" + name, 1), std::string("寫不出 ") + name);
    write_text(folder + "/notes.txt", "不是圖片");
    write_text(folder + "/vector.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
    write_text(folder + "/clip.mp4", "不是影片");

    // ---- 哪些檔案、怎麼排
    const auto names = [](const std::vector<std::string>& selection) {
        std::vector<std::string> result;
        for (const std::string& path : ImageSequence::files(selection)) result.push_back(file_part(path));
        return result;
    };
    const std::vector<std::string> expected = {"1.png", "2.png", "10.png", "A.jpg", "b.JPG"};
    const std::vector<std::string> whole = names({folder});
    c.expect(whole == expected, "選資料夾：得到 " + listed(whole) + "，預期 " + listed(expected));
    // 一個一個點檔案、順序亂的、同一個檔案又隨資料夾再選到一次、夾著不是圖片的與不存在的：結果一樣
    const std::vector<std::string> mixed = names({folder + "/10.png", folder + "/notes.txt", folder, folder + "/2.png", folder + "/不存在.png"});
    c.expect(mixed == expected, "混著選：得到 " + listed(mixed));
    const std::vector<std::string> picked = names({folder + "/b.JPG", folder + "/10.png", folder + "/2.png"});
    c.expect(picked == std::vector<std::string>({"2.png", "10.png", "b.JPG"}), "只選三個檔案：得到 " + listed(picked));
    c.expect(names({inner}) == std::vector<std::string>({"inner.png"}), "選子資料夾：得到 " + listed(names({inner})));
    c.expect(names({folder + "/notes.txt", folder + "/vector.svg", folder + "/clip.mp4", folder + "/folder.png"}).empty(), "不是圖片（或讀不進來的類型）的檔案被當成圖片");
    c.expect(names({folder + "/.hidden.png"}) == std::vector<std::string>({".hidden.png"}), "隱藏的檔案自己點了它就算（只有從資料夾裡找的時候才略過）");
    // 同一個檔案的幾種寫法只算一次
    c.expect(ImageSequence::files({folder + "/2.png", folder + "/./2.png", folder + "//2.png", inner + "/../2.png"}).size() == 1, "同一個檔案寫法不同時被算了好幾次");
    c.expect(ImageSequence::files({}).empty() && ImageSequence::files({work + "/沒有這個資料夾"}).empty(), "什麼都沒選、選到不存在的東西：沒有圖片");
    // 檔名的順序
    const auto before = [](const char* a, const char* b) { return ImageSequence::natural_less(a, b) && !ImageSequence::natural_less(b, a); };
    c.expect(before("2.jpg", "10.jpg") && before("pic9.png", "pic10.png") && before("IMG_0002.JPG", "img_0010.jpg") && before("a2b", "a10a"), "數字要照大小排（2 在 10 前面）");
    c.expect(before("A.jpg", "b.JPG") && before("a.jpg", "B.jpg") && before("apple", "Banana"), "不分大小寫");
    c.expect(before("pic", "pic1") && before("1.png", "a.png") && before("x007", "x8") && before("x7", "x08"), "短的在前、數字在字母前、前面的 0 不影響大小");
    c.expect(!ImageSequence::natural_less("same.png", "same.png") && (ImageSequence::natural_less("x07", "x7") != ImageSequence::natural_less("x7", "x07")), "一樣的不分先後；只差在前面的 0 時順序要固定");
    c.expect(ImageSequence::is_image("photo.JPEG") && ImageSequence::is_image("C:\\照片\\a.b.Png") && !ImageSequence::is_image("jpg") && !ImageSequence::is_image("dir.png/file") &&
                 !ImageSequence::is_image("photo.heic") && !ImageSequence::is_image("photo."),
             "只看副檔名認圖片");
    c.info("資料夾裡 " + std::to_string(whole.size()) + " 張：" + listed(whole));

    // ---- 記載的方向：存進去的像素不動，另外記「看的時候要怎麼轉」。讀進來之後，四個角落的顏色要落在該在的地方
    const Picture marks = corner_marks();
    // 轉正之後的左上、右上、左下、右下，各是存檔裡的哪一個角落（0 左上、1 右上、2 左下、3 右下）
    const std::array<int, 4> upright[9] = {{0, 1, 2, 3}, {0, 1, 2, 3}, {1, 0, 3, 2}, {3, 2, 1, 0}, {2, 3, 0, 1}, {0, 2, 1, 3}, {2, 0, 3, 1}, {3, 1, 2, 0}, {1, 3, 0, 2}};
    std::string turned;
    for (int orientation = 0; orientation <= 8; ++orientation) {
        // 0：檔案裡沒有記方向。EXIF 的兩種位元組順序、兩種位置輪流用
        const std::string path = inner + "/turn" + std::to_string(orientation) + ".jpg";
        if (!demo::write_picture_jpeg(marks, path, 10, orientation, orientation % 2 == 0, orientation % 3 == 0)) {
            c.expect(false, "寫不出 " + file_part(path));
            continue;
        }
        const SourceFramePtr loaded = ImageSequence::load(path), full = load_image_file(path);
        if (!loaded || !full) {
            c.expect(false, file_part(path) + " 讀不進來");
            continue;
        }
        const bool sideways = orientation >= 5;
        const int width = (sideways ? kImageHeight : kImageWidth) * 10, height = (sideways ? kImageWidth : kImageHeight) * 10;
        const std::array<int, 4> corners = corner_colors(*loaded);
        const auto text = [](const std::array<int, 4>& value) { return std::to_string(value[0]) + std::to_string(value[1]) + std::to_string(value[2]) + std::to_string(value[3]); };
        c.expect(corners == upright[orientation] && corner_colors(*full) == upright[orientation],
                 "方向 " + std::to_string(orientation) + "：四個角落是 " + text(corners) + "，預期 " + text(upright[orientation]));
        c.expect(loaded->shown_width() == width && loaded->shown_height() == height && full->width == width && full->height == height,
                 "方向 " + std::to_string(orientation) + "：大小 " + std::to_string(loaded->shown_width()) + "×" + std::to_string(loaded->shown_height()) + "，預期 " + std::to_string(width) + "×" + std::to_string(height));
        // 960×720 的圖不整張留著：短邊留 384 像素（輸出的四倍）就夠
        c.expect(std::min(loaded->width, loaded->height) == ImageSequence::kShortSidePixels && std::max(loaded->width, loaded->height) == 512,
                 "方向 " + std::to_string(orientation) + "：讀進來是 " + std::to_string(loaded->width) + "×" + std::to_string(loaded->height) + "，短邊預期 384");
        if (orientation == 6) turned = "直拍的相片（方向 6）960×720 → " + std::to_string(loaded->shown_width()) + "×" + std::to_string(loaded->shown_height());
    }
    if (!turned.empty()) c.info(turned + "；方向 1–8 與沒有記方向的都對");
    // 手機直拍的相片那麼大的圖（4032×3024、記著方向 6）：讀進來對不對，要多久（解開、轉正、縮小）
    {
        const std::string path = inner + "/phone.jpg";
        if (demo::write_picture_jpeg(marks, path, 42, 6)) {
            const double began = uptime_seconds();
            const SourceFramePtr photo = ImageSequence::load(path);
            const double spent = uptime_seconds() - began;
            c.expect(photo && photo->shown_width() == 3024 && photo->shown_height() == 4032 && photo->width == 384 && photo->height == 512 && corner_colors(*photo) == upright[6],
                     "4032×3024 的直拍相片讀進來不對" + (photo ? "（" + std::to_string(photo->width) + "×" + std::to_string(photo->height) + "）" : std::string("（讀不進來）")));
            c.info("4032×3024、記著方向 6 的測試圖讀進來（解開、轉正、縮小成 384×512）花了 " + number(spent * 1000, 0) + " 毫秒");
        } else {
            c.expect(false, "寫不出 phone.jpg");
        }
    }
    // EXIF 那一段的幾種情形：前面先有別的 APP1（XMP）、資料被截斷、根本不是 JPEG
    {
        std::vector<std::uint8_t> bytes;
        nbtv::tools::read_file(inner + "/turn6.jpg", bytes);
        c.expect(bytes.size() > 100 && jpeg_orientation(bytes.data(), bytes.size()) == 6, "從位元組讀方向");
        std::vector<std::uint8_t> with_xmp(bytes.begin(), bytes.begin() + 2);
        const std::uint8_t other[] = {0xFF, 0xE1, 0x00, 0x0A, 'h', 't', 't', 'p', ':', '/', '/', 'n'};   // 一段 APP1，內容不是 EXIF
        with_xmp.insert(with_xmp.end(), std::begin(other), std::end(other));
        with_xmp.insert(with_xmp.end(), bytes.begin() + 2, bytes.end());
        c.expect(jpeg_orientation(with_xmp.data(), with_xmp.size()) == 6, "EXIF 前面先有另一段 APP1（XMP）時也要找得到");
        c.expect(jpeg_orientation(with_xmp.data(), 24) == 1 && jpeg_orientation(bytes.data(), 3) == 1 && jpeg_orientation(nullptr, 0) == 1, "資料被截斷時當成沒有記方向，不能出事");
        const std::uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
        c.expect(jpeg_orientation(png, sizeof png) == 1, "不是 JPEG 的檔案沒有方向");
    }

    // ---- 大小：小圖不放大；大圖縮小之後內容還是那一張；很扁的圖照短邊算
    if (const SourceFramePtr small = ImageSequence::load(folder + "/1.png")) {
        c.expect(small->width == kImageWidth && small->height == kImageHeight && small->shown_width() == kImageWidth, "96×72 的小圖讀進來變成 " + std::to_string(small->width) + "×" + std::to_string(small->height));
        c.expect(demo::psnr(scale_frame(*small, ScaleMode::fill), coffee) > 60, "96×72 的圖讀進來再縮成 96×72，應該和原圖一模一樣");
    } else {
        c.expect(false, "1.png 讀不進來");
    }
    c.expect(demo::write_picture_png(coffee, inner + "/large.png", 10), "寫不出 large.png");
    if (const SourceFramePtr large = ImageSequence::load(inner + "/large.png")) {
        const double same = demo::psnr(scale_frame(*large, ScaleMode::fill), coffee);
        c.expect(large->width == 512 && large->height == 384 && large->shown_width() == 960 && large->shown_height() == 720, "960×720 的圖讀進來應該縮成 512×384");
        c.expect(same > 30, "大圖縮小之後再縮成 96×72，應該還是原來那一張（PSNR " + number(same, 1) + " dB）");
        c.info("960×720 的圖縮成 " + std::to_string(large->width) + "×" + std::to_string(large->height) + "，再縮成 96×72 和原圖差 " + number(same, 1) + " dB");
    } else {
        c.expect(false, "large.png 讀不進來");
    }
    {
        // 4000×500 的橫幅：由左到右從黑到白。縮成 3072×384 之後，每一點還是該有的亮度
        const int width = 4000, height = 500;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) std::fill_n(pixels.begin() + (static_cast<std::ptrdiff_t>(y) * width + x) * 3, 3, static_cast<std::uint8_t>(x * 255 / (width - 1)));
        }
        const std::string path = inner + "/wide.png";
        c.expect(write_png(path, pixels.data(), width, height, 3), "寫不出 wide.png");
        const SourceFramePtr wide = ImageSequence::load(path);
        c.expect(wide && wide->width == 3072 && wide->height == 384 && wide->shown_width() == width && wide->shown_height() == height,
                 "4000×500 的圖應該縮成 3072×384，得到 " + (wide ? std::to_string(wide->width) + "×" + std::to_string(wide->height) : std::string("讀不進來")));
        if (wide && wide->width == 3072) {
            bool smooth = true;
            for (const int x : {0, 1, 700, 1536, 2500, 3070, 3071}) {
                for (const int y : {0, 191, 383}) {
                    const double wanted = (x + 0.5) * width / 3072.0 * 255.0 / (width - 1);
                    const int got = wide->rgb[(static_cast<std::size_t>(y) * 3072 + static_cast<std::size_t>(x)) * 3 + 1];
                    smooth = smooth && std::fabs(got - wanted) <= 2.0;
                }
            }
            c.expect(smooth, "縮小之後的亮度不對（每一點應該是它蓋到的那一塊的平均）");
        }
        // 短邊不到 384 的扁圖不放大
        std::vector<std::uint8_t> strip(static_cast<std::size_t>(2000) * 100 * 3, 90);
        c.expect(write_png(inner + "/strip.png", strip.data(), 2000, 100, 3), "寫不出 strip.png");
        const SourceFramePtr thin = ImageSequence::load(inner + "/strip.png");
        c.expect(thin && thin->width == 2000 && thin->height == 100, "短邊不到 384 的圖不應該放大");
    }
    // 壞掉的檔案（副檔名是圖片、內容不是）讀不進來
    write_text(folder + "/broken.png", "這不是 PNG");
    c.expect(ImageSequence::is_image(folder + "/broken.png") && ImageSequence::load(folder + "/broken.png") == nullptr, "壞掉的圖片檔沒有被認出來");
    // 最近讀過的幾張直接給；換一批之後要重讀
    ImageSequence cache;
    const SourceFramePtr first = cache.picture(folder + "/1.png");
    c.expect(first != nullptr && cache.picture(folder + "/1.png") == first, "剛讀過的那一張應該直接給同一份");
    cache.picture(folder + "/2.png");
    cache.picture(folder + "/10.png");
    c.expect(cache.picture(folder + "/1.png") == first, "留三張：第一張還在");
    cache.picture(folder + "/A.jpg");
    cache.picture(folder + "/b.JPG");
    c.expect(cache.picture(folder + "/2.png") != nullptr && cache.picture(folder + "/1.png") != first, "超過三張之後最舊的要丟掉、重讀");
    cache.forget();
    c.expect(cache.picture(folder + "/不存在.png") == nullptr, "讀不到的檔案回傳空的");
}

/// 用圖片序列當來源的整個發射流程。用 app 真正的狀態物件，音訊輸出換成模擬的、PTT 換成記錄用的：
///   * 選了資料夾，預覽是第一張；按下發射，照檔名的順序一張一張送，每張送一次，送完自動停止、放開 PTT，位置回到第一張
///   * 送到一半按停止：這一張送完就停；已經取走、還沒送出去的那一張退回去，再按發射從它接著送到最後一張（沒有哪一張被跳過或重複）
///   * 立刻停止（最後一張被切掉）：被切掉的那一張下一次重送
///   * 還沒開始送就取消：第一張沒有被跳過
///   * 勾了循環播放：送完最後一張再從第一張來，不會自己停
///   * 只有一張：送一張就停
///   * 讀不到的檔案從序列裡拿掉、在底列說明，其餘的照送；都讀不到就不發射
///   * 上次選的那一批（設定裡記的路徑）啟動時自動帶回來，不見了的檔案略過
///   * 把檔案、資料夾拖進視窗
void test_image_transmit(Check& c, const std::string& work) {
    const std::string folder = work + "/序列 sequence";
    SDL_CreateDirectory(folder.c_str());
    // 檔名故意讓「照字元排」和「數字照大小排」的結果不同：正確的順序是 2、3、10、11
    const std::vector<std::string> file_names = {"pic2.png", "pic3.png", "pic10.png", "pic11.png"};
    const std::vector<Picture> pictures = {picture_or_gray("coffee"), picture_or_gray("rocket"), picture_or_gray("cat"), color_bars()};
    std::vector<Picture> references;
    for (std::size_t i = 0; i < file_names.size(); ++i) {
        const std::string path = folder + "/" + file_names[i];
        const SourceFramePtr loaded = demo::write_picture_png(pictures[i], path, 4) ? ImageSequence::load(path) : nullptr;
        if (!loaded) return c.expect(false, "測試用的圖片檔做不出來：" + file_names[i]);
        references.push_back(scale_frame(*loaded, ScaleMode::fill));
    }
    /// 一張收到的畫面最像哪一張圖片（0 起算）
    const auto nearest = [&references](const Picture& picture) {
        int best = 0;
        for (int i = 1; i < static_cast<int>(references.size()); ++i) {
            if (demo::psnr(picture, references[static_cast<std::size_t>(i)]) > demo::psnr(picture, references[static_cast<std::size_t>(best)])) best = i;
        }
        return best;
    };
    const auto text = [](const std::vector<int>& order) {
        std::string joined;
        for (const int index : order) joined += (joined.empty() ? "" : "、") + std::to_string(index + 1);
        return joined.empty() ? std::string("沒有") : joined;
    };
    /// from、from+1、…、to−1
    const auto counted = [](int from, int to) {
        std::vector<int> list;
        for (int i = from; i < to; ++i) list.push_back(i);
        return list;
    };

    Rig rig(12);   // 模式 12：每張 1.04 秒
    App& app = *rig.app;
    std::vector<std::string> traces;
    const std::function<void(const std::string&)> previous_trace = App::trace;
    App::trace = [&traces](const std::string& line) { traces.push_back(line); };
    const auto traced = [&traces](const char* part) {
        for (const std::string& line : traces) {
            if (contains(line, part)) return true;
        }
        return false;
    };
    const auto preview_is = [&](std::size_t index) { return app.source_preview != nullptr && demo::psnr(*app.source_preview, references[index]) > 45; };

    app.set_source_kind(SourceKind::images);
    c.expect(app.source_message.has_value() && !app.can_transmit(), "還沒選圖片時不能發射，預覽的地方要有說明");
    c.expect(app.open_images({folder}, false) == 4 && listed(app.image_files) == listed(file_names), "選資料夾之後的順序是 " + listed(app.image_files));
    if (!rig.wait(10, [&] { return app.can_transmit() && app.source_preview != nullptr; })) {
        App::trace = previous_trace;
        return c.expect(false, "選了圖片之後一直沒有來源畫面");
    }
    c.expect(app.image_index == 0 && preview_is(0) && app.source_size == std::optional<std::pair<int, int>>(std::make_pair(kImageWidth * 4, kImageHeight * 4)) && !app.source_message.has_value(),
             "選了圖片之後，預覽不是第一張");
    c.expect(app.images_play_once() && !app.video_plays_once(), "沒有勾循環播放：送完會自動停止");
    // 位置移到第三張：預覽跟著換（預覽是「下一張要送的」）
    app.select_image(2);
    c.expect(rig.wait(5, [&] { return preview_is(2); }) && app.next_image() == std::optional<std::string>(app.image_files[2]), "位置移到第三張，預覽沒有跟著換");
    app.select_image(99);
    c.expect(app.image_index == 3, "位置超出範圍時沒有停在最後一張");
    app.select_image(-5);
    c.expect(app.image_index == 0, "位置是負的時候沒有停在第一張");

    /// 等這一次發射結束，回傳解回來的每一張各是第幾張圖片；label 用在出問題時的說明。
    /// expect_notice：這一次底列本來就該有訊息；cut_short：最後一張是被切掉的，收到解不出來的殘幀不算問題。
    const auto finish = [&](const std::string& label, bool expect_notice = false, bool cut_short = false) {
        if (!rig.wait(25, [&] { return app.tx_phase == App::TxPhase::idle; })) {
            c.expect(false, label + "：發射沒有結束");
            app.abort_transmit();
            rig.wait(10, [&] { return app.tx_phase == App::TxPhase::idle; });
        }
        rig.wait(3, [&] { return rig.ptt->keys().size() % 2 == 0; });   // PTT 的放開在另一條執行緒上做
        const std::vector<bool> keyed = rig.ptt->take();
        c.expect(keyed.empty() || keyed == std::vector<bool>({true, false}), label + "：PTT 不是「按下一次、放開一次」");
        if (!expect_notice) c.expect(!app.notice.has_value(), label + "：" + app.notice.value_or(""));
        std::vector<int> order;
        int good = 0, total = 0;
        for (const DecodedFrame& frame : decode(rig.sink->drain())) {
            ++total;
            if (!frame.decoded || !frame.header_ok || frame.callsign != "BX4ACP" || !frame.picture) continue;
            ++good;
            order.push_back(nearest(*frame.picture));
        }
        if (!cut_short) c.expect(good == total, label + "：收到 " + std::to_string(total) + " 張，其中解得出來的只有 " + std::to_string(good) + " 張");
        return order;
    };
    const auto begin = [&] {
        rig.sink->drain();
        rig.ptt->take();
        traces.clear();
        app.start_transmit();
    };
    const auto how_dropped = [&] { return traced("已經排進佇列但還沒開始播") ? "已經排進佇列" : traced("剛編好：不送") ? "還在編碼" : "不明"; };

    // 1. 從頭送到尾：四張各一次，送完自己停，位置回到第一張
    begin();
    std::vector<int> order = finish("從頭送到尾");
    c.expect(order == std::vector<int>({0, 1, 2, 3}) && app.tx_frames_sent == 4, "從頭送到尾：送出 " + std::to_string(app.tx_frames_sent) + " 張，解回來的是第 " + text(order) + " 張，預期 1、2、3、4");
    c.expect(app.image_index == 0, "送完之後位置沒有回到第一張（在第 " + std::to_string(app.image_index + 1) + " 張）");
    c.expect(traced("圖片序列送完"), "送完最後一張應該自己收尾");
    c.info("四張照檔名的順序各送一次、自動停止");

    // 2. 第二張送到後半才按停止：第三張這時已經取走（編好排進佇列，或還在編），不會送出去，要退回去
    begin();
    if (rig.wait(15, [&] { return app.tx_phase == App::TxPhase::transmitting && app.image_index == 3; })) {
        demo::spin(&app, 0.25);
        app.stop_transmit();
        const int rolled_back = app.image_index;
        order = finish("送到一半按停止");
        const std::string how = how_dropped();
        // 平常這時候正在送第二張：送出的是 1、2，第三張退回去。編碼很慢的機器上下一張準備得早，按停止時可能還在送第一張
        // （那就只送出 1，後面取走的都退回去）。不管哪一種：送出去的是從頭數起連著的幾張，位置停在第一張還沒送的
        const int sent = static_cast<int>(order.size());
        c.expect(sent >= 1 && sent <= 2 && order == counted(0, sent), "送到一半按停止：解回來的是第 " + text(order) + " 張，預期 1、2");
        c.expect(rolled_back == sent && app.image_index == sent,
                 "送到一半按停止：取走卻沒送出去的那一張沒有退回去（送出 " + std::to_string(sent) + " 張，按停止之後位置在第 " + std::to_string(rolled_back + 1) + " 張，結束時在第 " + std::to_string(app.image_index + 1) + " 張）");
        // 預覽也跟著回到那一張
        c.expect(sent < 4 && rig.wait(5, [&] { return preview_is(static_cast<std::size_t>(std::min(sent, 3))); }), "送到一半按停止：預覽沒有回到下一張要送的那一張");
        // 再按一次：從那一張接著送到最後一張
        begin();
        const std::vector<int> rest = finish("停下之後再按一次");
        c.expect(rest == counted(sent, 4) && app.image_index == 0, "停下之後再按一次：解回來的是第 " + text(rest) + " 張，預期 " + text(counted(sent, 4)));
        c.info("第二張送到後半按停止（下一張" + how + "）→ 只送出 " + text(order) + "，再按一次接著送 " + text(rest));
    } else {
        c.expect(false, "送到一半按停止：一直沒有送到第二張");
        app.abort_transmit();
        finish("送到一半按停止", true, true);
    }

    // 3. 第二張才剛取走就按停止（多半還在編碼）：第一張送完就停，第二張退回去
    app.notice.reset();
    app.select_image(0);
    begin();
    if (rig.wait(15, [&] { return app.tx_phase == App::TxPhase::transmitting && app.image_index == 2; })) {
        app.stop_transmit();
        const int rolled_back = app.image_index;
        order = finish("剛取走下一張就按停止");
        const std::string how = how_dropped();
        // 平常第一張正在送：只送出 1，第二張退回去。很慢的機器上第一張可能還沒開始播就被拿掉，那就一張都沒送、位置回到第一張
        const int sent = static_cast<int>(order.size());
        c.expect(sent <= 1 && order == counted(0, sent) && rolled_back == sent && app.image_index == sent,
                 "剛取走下一張就按停止：解回來的是第 " + text(order) + " 張（預期 1），位置在第 " + std::to_string(app.image_index + 1) + " 張（預期 " + std::to_string(sent + 1) + "）");
        c.info("剛取走第二張就按停止（第二張" + how + "）→ 只送出 " + text(order) + "，位置回到 " + std::to_string(app.image_index + 1));
    } else {
        c.expect(false, "剛取走下一張就按停止：一直沒有取到第二張");
        app.abort_transmit();
        finish("剛取走下一張就按停止", true, true);
    }

    // 4. 立刻停止：正在送的第二張被切掉，對方解不出來；下一次從它重送
    app.notice.reset();
    app.select_image(0);
    begin();
    if (rig.wait(15, [&] { return app.tx_phase == App::TxPhase::transmitting && app.tx_frame_number == 2; })) {
        app.abort_transmit();
        const int rolled_back = app.image_index;
        order = finish("立刻停止", true, true);
        c.expect(order == std::vector<int>({0}) && rolled_back == 1, "立刻停止：解回來的是第 " + text(order) + " 張（預期 1），位置在第 " + std::to_string(rolled_back + 1) + " 張（預期 2）");
        c.info("第二張送到一半立刻停止 → 位置回到 2");
    } else {
        c.expect(false, "立刻停止：一直沒有送到第二張");
        app.abort_transmit();
        finish("立刻停止", true, true);
    }
    app.notice.reset();

    // 5. 還沒開始送就取消。先是按下發射馬上又按停止；再來讓 PTT 慢一點，等第一張已經取走（位置移到第二張）、還在等 PTT 時取消
    app.select_image(0);
    begin();
    app.stop_transmit();
    demo::spin(&app, 0.5);
    c.expect(app.tx_phase == App::TxPhase::idle && app.image_index == 0, "按下發射馬上取消：位置跑到第 " + std::to_string(app.image_index + 1) + " 張");
    rig.ptt->press_delay = 0.6;
    begin();
    if (rig.wait(10, [&] { return app.tx_phase == App::TxPhase::starting && app.image_index == 1; })) {
        app.stop_transmit();
        c.expect(app.tx_phase == App::TxPhase::idle && app.image_index == 0, "等 PTT 的時候取消：取走的第一張沒有退回去（位置在第 " + std::to_string(app.image_index + 1) + " 張）");
    } else {
        c.expect(false, "等 PTT 的時候取消：沒有等到「第一張已經取走、PTT 還沒按好」的時刻");
    }
    rig.ptt->press_delay = 0.0;
    rig.wait(10, [&] { return app.tx_phase == App::TxPhase::idle; });
    demo::spin(&app, 1.0);   // 等那一次慢的 PTT 按下、放開都做完
    {
        const std::vector<bool> keyed = rig.ptt->take();
        c.expect(keyed.empty() || !keyed.back(), "還沒開始送就取消：PTT 還按著");
    }
    c.expect(rig.sink->drain().empty(), "還沒開始送就取消：不應該送出任何聲音");
    app.notice.reset();
    c.info("還沒開始送就取消，第一張沒有被跳過");

    // 6. 循環播放：送完第四張再從第一張來，不會自己停
    app.settings.loop_images = true;
    c.expect(!app.images_play_once(), "勾了循環播放：不會自己停");
    app.select_image(0);
    begin();
    if (rig.wait(20, [&] { return app.tx_phase == App::TxPhase::idle || app.tx_frames_sent >= 5; }) && app.tx_phase == App::TxPhase::transmitting) {
        app.stop_transmit();
        order = finish("循環播放");
        c.expect(order.size() >= 5 && std::vector<int>(order.begin(), order.begin() + 5) == std::vector<int>({0, 1, 2, 3, 0}), "循環播放：解回來的是第 " + text(order) + " 張，預期 1、2、3、4、1…");
        c.info("循環播放時送了第 " + text(order) + " 張，按停止才結束");
    } else {
        c.expect(false, "循環播放：送不到五張就停了");
        finish("循環播放", true, true);
    }
    app.settings.loop_images = false;
    app.notice.reset();

    // 7. 只選一張：送一張就停
    if (app.open_images({folder + "/" + file_names[1]}, false) != 1 || !rig.wait(10, [&] { return app.can_transmit() && preview_is(1); })) {
        c.expect(false, "只選一張：沒有來源畫面");
    } else {
        begin();
        order = finish("只選一張");
        c.expect(order == std::vector<int>({1}) && app.tx_frames_sent == 1, "只選一張：送出 " + std::to_string(app.tx_frames_sent) + " 張，解回來的是第 " + text(order) + " 張");
        c.info("只有一張時送一張就停");
    }

    // 8. 讀不到的檔案：排在最前面的在預覽時就拿掉；排在中間的在輪到它時拿掉，其餘的照送
    const std::string mixed = folder + "/mixed";
    SDL_CreateDirectory(mixed.c_str());
    write_text(mixed + "/a-broken.png", "壞掉的圖片");
    write_text(mixed + "/c-broken.png", "壞掉的圖片");
    demo::write_picture_png(pictures[0], mixed + "/b.png", 4);
    demo::write_picture_png(pictures[3], mixed + "/d.png", 4);
    app.notice.reset();
    c.expect(app.open_images({mixed}, false) == 4, "讀不到的檔案：一開始不是四個檔案");
    if (rig.wait(10, [&] { return app.can_transmit() && app.image_files.size() == 3; }) && app.notice && contains(*app.notice, "a-broken.png")) {
        app.notice.reset();
        begin();
        order = finish("讀不到的檔案", true);
        c.expect(order == std::vector<int>({0, 3}) && listed(app.image_files) == "b.png、d.png" && app.notice && contains(*app.notice, "c-broken.png"),
                 "讀不到的檔案：解回來的是第 " + text(order) + " 張（預期 1、4），序列剩 " + listed(app.image_files) + "，訊息「" + app.notice.value_or("無") + "」");
        c.info("讀不到的兩個檔案從序列裡拿掉，其餘兩張照送");
    } else {
        c.expect(false, "讀不到的檔案：排在最前面的壞檔沒有在預覽時被拿掉（序列 " + std::to_string(app.image_files.size()) + " 個，訊息「" + app.notice.value_or("無") + "」）");
    }
    // 都讀不到：序列變成空的、預覽的地方說明、不能發射；硬要發射也不會按下 PTT
    const std::string broken = folder + "/broken";
    SDL_CreateDirectory(broken.c_str());
    write_text(broken + "/x.png", "壞掉的圖片");
    write_text(broken + "/y.jpg", "壞掉的圖片");
    app.notice.reset();
    c.expect(app.open_images({broken}, false) == 2, "都讀不到：一開始不是兩個檔案");
    c.expect(rig.wait(10, [&] { return app.image_files.empty(); }) && app.source_message.has_value() && !app.can_transmit() && app.notice.has_value(), "都讀不到：序列應該變成空的，並說明原因");
    rig.ptt->take();
    app.start_transmit();
    c.expect(rig.wait(10, [&] { return app.tx_phase == App::TxPhase::idle; }) && rig.ptt->take().empty() && rig.sink->drain().empty(), "沒有圖片可以送的時候不能按下 PTT、不能送出聲音");
    // 選的東西裡沒有圖片：維持原來那一批，在底列說明
    app.notice.reset();
    c.expect(app.open_images({folder}, false) == 4 && app.open_images({broken + "/沒有.txt"}, false) == 0 && app.image_files.size() == 4 && app.notice.has_value(), "選的東西裡沒有圖片：要維持原來那一批並說明");
    app.notice.reset();
    App::trace = previous_trace;

    // 9. 上次選的那一批：設定裡記著路徑，下次啟動時自動帶回來（已經不在的檔案略過）。
    //    NBTV_NO_DEVICES：照平常的方式啟動，但不開音訊輸入與鏡頭
    {
        const bool had = std::getenv("NBTV_NO_DEVICES") != nullptr;
        SDL_setenv_unsafe("NBTV_NO_DEVICES", "1", 1);
        Settings remembered = Settings::load("");
        remembered.source_kind = SourceKind::images;
        for (const std::string& name : file_names) remembered.image_paths.push_back(folder + "/" + name);
        remembered.image_paths.push_back(folder + "/不見了.png");
        App again(std::move(remembered), true);
        again.start();
        c.expect(listed(again.image_files) == listed(file_names) && demo::wait_until(&again, 10, [&] { return again.can_transmit(); }),
                 "啟動時沒有把上次選的圖片帶回來（" + std::to_string(again.image_files.size()) + " 張）");
        again.shutdown();
        if (!had) SDL_unsetenv_unsafe("NBTV_NO_DEVICES");
        // 自己選的那一批會記進設定；程式自己帶回來的、測試給的不會
        Rig chooser(12);
        chooser.app->open_images({folder}, true);
        c.expect(chooser.app->settings.image_paths.size() == 4 && file_part(chooser.app->settings.image_paths[2]) == "pic10.png", "選的那一批應該記進設定");
        c.info("上次選的四張啟動時自動帶回來");
    }

    // 10. 拖進視窗的檔案：資料夾或好幾張圖片是圖片序列；一張圖片照舊當成在「影片檔」那裡選了它，來源已經是圖片序列時才算序列
    {
        Rig drop(12);
        App& target = *drop.app;
        target.open_dropped({folder});
        c.expect(target.settings.source_kind == SourceKind::images && target.image_files.size() == 4 && target.settings.image_paths.size() == 4, "拖進資料夾：應該變成圖片序列");
        target.open_dropped({folder + "/" + file_names[3]});
        c.expect(target.settings.source_kind == SourceKind::images && listed(target.image_files) == "pic11.png", "來源是圖片序列時拖進一張圖片：序列換成那一張");
        target.set_source_kind(SourceKind::camera);
        target.open_dropped({folder + "/" + file_names[2], folder + "/" + file_names[0]});
        c.expect(target.settings.source_kind == SourceKind::images && listed(target.image_files) == "pic2.png、pic10.png", "拖進兩張圖片：應該變成圖片序列，順序照檔名");
        target.set_source_kind(SourceKind::camera);
        target.open_dropped({folder + "/" + file_names[1]});
        c.expect(target.settings.source_kind == SourceKind::video && drop.wait(10, [&] { return target.video_name.has_value(); }) && target.video_is_image && target.video_name == std::optional<std::string>("pic3.png"),
                 "來源不是圖片序列時拖進一張圖片：照舊當成在「影片檔」那裡選了它");
        c.expect(listed(target.image_files) == "pic2.png、pic10.png", "換成影片檔之後，原來那一批圖片還在");
        // 拖進一個沒有圖片的資料夾：來源不動，在底列說明
        const std::string empty = folder + "/empty";
        SDL_CreateDirectory(empty.c_str());
        target.notice.reset();
        target.open_dropped({empty});
        c.expect(target.settings.source_kind == SourceKind::video && target.notice.has_value() && target.video_name == std::optional<std::string>("pic3.png") && target.image_files.size() == 2,
                 "拖進沒有圖片的資料夾：來源與原來選的東西都不應該動，並說明原因");
        target.shutdown();
    }
}

void test_ui_render(Check& c, const std::string& work) {
    const std::string original = i18n::language();
    const std::unique_ptr<App> receiving = snapshot::make_receiving_app(true);
    const std::unique_ptr<App> images = snapshot::make_images_app(work);   // 待機，來源是圖片序列（三張，下一張要送的是第二張）
    c.expect(images->image_files.size() == 3 && images->image_index == 1 && images->source_preview != nullptr, "示範用的圖片序列沒有準備好");
    {
        OffscreenUI screen(*receiving, 1280, 840, 1.0f);
        if (!screen.ok()) return c.expect(false, "離屏繪圖起不來：" + screen.error());
        screen.frames(3);
        c.expect(screen.distinct_colors() > 30, "畫面不應該是一片空白（顏色 " + std::to_string(screen.distinct_colors()) + " 種）");
        if (!screen.ui().has_any_font()) c.skip("找不到任何字型檔", "fonts");
        else if (!screen.ui().has_cjk_fonts()) c.skip("找不到中文、日文的字型", "cjk");
        c.expect(screen.ui().rect_of("##transmit").has_value() && screen.ui().rect_of("##settings").has_value() && screen.ui().rect_of("##thumb/5").has_value(), "按鈕、縮圖應該都畫出來了");
        // 五種語言、各個面板都畫一次
        for (const std::string& language : i18n::languages()) {
            i18n::use(language);
            screen.reload_fonts();
            screen.frames(2);
            screen.ui().show_settings = true;
            screen.frames(3);
            c.expect(screen.ui().rect_of("##closesettings").has_value() && screen.ui().rect_of("##pttmode/serial").has_value(), language + "：設定沒有畫出來");
            screen.ui().show_settings = false;
            screen.frames(2);
            screen.ui().open_mode_menu();
            screen.frames(3);
            c.expect(screen.ui().rect_of("##moderow/11").has_value(), language + "：模式選單沒有畫出來");
            screen.press_escape();
            screen.ui().open_schedule_menu();
            screen.frames(3);
            c.expect(screen.ui().rect_of("##armed").has_value(), language + "：定時發射的面板沒有畫出來");
            screen.press_escape();
            // 按了「儲存影片」之後的那一小塊：選項與「儲存…」鈕都要在視窗裡面
            screen.ui().open_save_video_menu();
            screen.frames(3);
            const std::optional<UI::HitRect> option = screen.ui().rect_of("##stampsnr"), go = screen.ui().rect_of("##savevideogo");
            c.expect(option && go && option->x >= 0 && option->x + option->w <= 1280 && go->x + go->w <= 1280 && go->y + go->h <= 840, language + "：存影片的選項沒有畫出來，或跑出視窗");
            screen.press_escape();
            // 來源是圖片序列時的右側欄：三個來源排成一列，預覽下面有上一張、下一張、滑桿，還有循環播放的勾。視窗縮到最小也都放得下
            for (const int width : {1280, 1040}) {
                const int height = width == 1280 ? 840 : 700;
                OffscreenUI side(*images, width, height, 1.0f);
                side.frames(3);
                const std::optional<UI::HitRect> third = side.ui().rect_of("##source/2"), previous = side.ui().rect_of("##previmage"), next = side.ui().rect_of("##nextimage"),
                                                 slider = side.ui().rect_of("##imageposition"), loop = side.ui().rect_of("##loopimages"), choose = side.ui().rect_of("##chooseimages"),
                                                 button = side.ui().rect_of("##transmit"), level = side.ui().rect_of("##level"), scale = side.ui().rect_of("##scale/0");
                const std::string where = language + "（" + std::to_string(width) + "×" + std::to_string(height) + "）";
                c.expect(third && previous && next && slider && loop && choose && button && level && scale, where + "：圖片序列的控制元件沒有畫出來");
                if (!(third && previous && next && slider && loop && choose && button && level && scale)) continue;
                c.expect(third->x + third->w <= static_cast<float>(width) && choose->x + choose->w <= static_cast<float>(width) && slider->w > 40 && slider->x > next->x + next->w,
                         where + "：圖片序列的那幾列太擠或跑出視窗");
                c.expect(previous->y + previous->h <= loop->y && loop->y + loop->h <= level->y && loop->x + loop->w <= scale->x && level->y + level->h <= button->y + 1 && button->y + button->h < static_cast<float>(height),
                         where + "：右側欄的東西疊在一起或跑出視窗");
            }
        }
        i18n::use(original);
        screen.reload_fonts();
    }
    // 發射中、待機，以及比較小、比較大的視窗
    const std::unique_ptr<App> transmitting = snapshot::make_transmitting_app(receiving->history);
    const std::unique_ptr<App> idle = snapshot::make_idle_app("");
    idle->ptt_warning = "測試用的警告：PTT 可能還按著";
    idle->notice = "測試用的訊息";
    struct Size {
        int width, height;
        float density;
    };
    for (const Size size : {Size{1040, 700, 1.0f}, Size{1280, 840, 1.0f}, Size{1920, 1200, 1.5f}}) {
        for (App* app : {transmitting.get(), idle.get(), receiving.get(), images.get()}) {
            OffscreenUI screen(*app, size.width, size.height, size.density);
            screen.frames(3);
            const std::optional<UI::HitRect> button = screen.ui().rect_of("##transmit"), level = screen.ui().rect_of("##level");
            c.expect(screen.ok() && screen.distinct_colors() > 20, std::to_string(size.width) + "×" + std::to_string(size.height) + " 畫不出來");
            c.expect(button && level && button->y + button->h < static_cast<float>(size.height) && level->y + level->h <= button->y + 1 && level->y > 0,
                     std::to_string(size.width) + "×" + std::to_string(size.height) + "：右側欄的東西疊在一起或跑出視窗");
        }
    }
    // 很多張：只畫看得到的那幾列，其餘的捲動時才畫
    for (int i = 0; i < 300; ++i) {
        DecodedFrame frame;
        frame.mode_id = 12;
        frame.decoded = true;
        frame.header_ok = true;
        frame.callsign = "N0CALL";
        frame.sequence = i;
        frame.picture = std::make_shared<Picture>(picture_or_gray("cat"));
        receiving->add(frame, snapshot::time_today(15, 0, 0) + i * 1.04);
    }
    receiving->settings.show_frame_snr = true;
    OffscreenUI crowded(*receiving, 1280, 840, 1.0f);
    crowded.frames(3);
    c.expect(crowded.ui().rect_of("##thumb/0").has_value() && !crowded.ui().rect_of("##thumb/200").has_value(), "306 張時只畫看得到的那幾列");
}

void test_ui_click(Check& c) {
    const std::unique_ptr<App> app = snapshot::make_receiving_app(true);
    OffscreenUI screen(*app, 1280, 840, 1.0f);
    if (!screen.ok()) return c.expect(false, "離屏繪圖起不來：" + screen.error());
    screen.frames(3);
    // 設定：打開、改呼號、選 PTT、關掉
    c.expect(screen.click("##settings") && screen.ui().show_settings, "按設定鈕要打開設定");
    screen.frames(2);
    c.expect(screen.click("##pttmode/rigctld") && app->settings.ptt_mode == PTTMode::rigctld, "選 PTT 的方式");
    c.expect(screen.click("##callsign"), "點呼號的輸入框");
    screen.type_text("x");
    c.expect(app->settings.callsign == "BX4ACPX", "在呼號後面打一個字（自動轉大寫）：「" + app->settings.callsign + "」");
    c.expect(screen.click("##line/1") && app->settings.serial_line == SerialLine::dtr, "RTS／DTR 的切換");
    c.expect(screen.click("##snr") && app->settings.show_frame_snr, "勾「標示訊雜比」");
    c.expect(screen.click("##closesettings") && !screen.ui().show_settings, "按叉叉關掉設定");
    screen.frames(2);
    c.expect(screen.click("##settings") && screen.ui().show_settings, "再打開一次");
    screen.frames(2);
    c.expect(screen.click("##language"), "打開語言的選單");
    screen.press_escape();
    screen.frames(2);
    screen.press_escape();
    c.expect(!screen.ui().show_settings, "按 Esc 關掉設定");
    screen.frames(2);
    // 設定開著的時候底下的按鈕按不到
    screen.ui().show_settings = true;
    screen.frames(3);
    const SourceKind before = app->settings.source_kind;
    if (const std::optional<UI::HitRect> rect = screen.ui().rect_of("##scale/1")) screen.click(rect->x + rect->w / 2, rect->y + rect->h / 2);
    c.expect(app->settings.scale_mode == ScaleMode::fill && app->settings.source_kind == before, "設定開著的時候，底下的東西不能被按到");
    screen.ui().show_settings = false;
    screen.frames(3);
    // 發射模式的選單
    c.expect(screen.click("##mode"), "按發射模式的鈕");
    screen.frames(2);
    c.expect(screen.click("##moderow/12") && app->settings.tx_mode_id == 12, "在選單裡選模式 12");
    screen.frames(2);
    c.expect(!screen.ui().rect_of("##moderow/12").has_value(), "選了之後選單要關掉");
    // 右側欄
    c.expect(screen.click("##scale/1") && app->settings.scale_mode == ScaleMode::fit, "縮放方式：完整");
    c.expect(screen.click("##canvas") && app->settings.canvas_enabled && app->settings.canvas_text == "CQ BX4ACPX", "打開畫布，自動填上「CQ 呼號」：「" + app->settings.canvas_text + "」");
    c.expect(screen.click("##canvaspos/1") && app->settings.canvas_position == CanvasPosition::bottom, "畫布印在下緣");
    c.expect(screen.click("##source/1") && app->settings.source_kind == SourceKind::video, "來源：影片檔");
    c.expect(screen.click("##source/2") && app->settings.source_kind == SourceKind::images, "來源：圖片序列");
    screen.frames(2);
    c.expect(screen.click("##chooseimages"), "圖片序列的「選擇…」鈕");
    screen.frames(2);
    c.expect(screen.ui().rect_of("##imagesmenu/files").has_value() && screen.ui().rect_of("##imagesmenu/folder").has_value(), "按「選擇…」要問是選圖片檔還是選資料夾");
    keep_screen(screen, "images-choose-menu");
    screen.press_escape();
    screen.frames(2);
    c.expect(!screen.ui().rect_of("##imagesmenu/files").has_value() && !screen.ui().rect_of("##previmage").has_value(), "按 Esc 關掉那個選單；還沒選圖片時沒有上一張、下一張");
    c.expect(screen.click("##source/1") && app->settings.source_kind == SourceKind::video, "換回影片檔");
    // 最近收到：點第三張
    c.expect(screen.click("##thumb/2") && app->selected_id == std::optional<std::uint64_t>(app->history[2].id), "點縮圖看那一張");
    c.expect(screen.click("##thumb/0") && !app->selected_id.has_value(), "點最新的那張：回到跟著最新的");
    // 定時發射
    c.expect(screen.click("##schedule"), "按定時發射的鈕");
    screen.frames(2);
    c.expect(screen.click("##armed") && app->schedule_armed, "打開定時發射");
    c.expect(screen.click("##interval/4") && app->settings.schedule_interval_minutes == schedule::intervals()[4], "改間隔");
    c.expect(screen.click("##busy") && !app->settings.schedule_skip_when_busy, "「正在收別人的畫面時不發射」的勾");
    screen.press_escape();
    app->set_schedule_armed(false);
    screen.frames(2);
    // 存影片：按了先跳出選項，勾不勾當下就記進設定
    if (FileSource::ffmpeg_available()) c.expect(screen.click("##savevideo"), "按「儲存影片」");
    else screen.ui().open_save_video_menu();   // 沒有 ffmpeg 時那個鈕是灰的，按不下去
    screen.frames(2);
    c.expect(screen.ui().rect_of("##stampsnr").has_value() && screen.ui().rect_of("##savevideogo").has_value(), "按「儲存影片」要先跳出選項與「儲存…」鈕");
    c.expect(!app->settings.stamp_video_snr && screen.click("##stampsnr") && app->settings.stamp_video_snr, "勾「在每一張的左上角印上訊雜比」");
    c.expect(screen.click("##stampsnr") && !app->settings.stamp_video_snr, "再按一次取消");
    c.expect(screen.click("##stampsnr") && app->settings.stamp_video_snr, "再勾起來");
    keep_screen(screen, "save-video-option");
    screen.press_escape();
    screen.frames(2);
    c.expect(!screen.ui().rect_of("##stampsnr").has_value() && app->settings.stamp_video_snr && !app->exporting_video, "按 Esc 關掉：勾的留著，沒有開始存檔");
    if (FileSource::ffmpeg_available()) {
        screen.click("##savevideo");
        screen.frames(2);
        // 沒有視窗的時候開不了系統的存檔對話框：按「儲存…」只會把這一小塊關掉
        c.expect(screen.click("##savevideogo"), "按「儲存…」");
        screen.frames(2);
        c.expect(!screen.ui().rect_of("##stampsnr").has_value() && !app->exporting_video, "按了「儲存…」之後那一小塊要關掉");
    }
}

/// 介面：圖片序列的控制元件（上一張、下一張、滑桿、循環播放）真的有作用，預覽跟著換。
void test_ui_images(Check& c, const std::string& work) {
    const std::string folder = work + "/介面 ui-images";
    SDL_CreateDirectory(folder.c_str());
    const std::vector<Picture> pictures = {picture_or_gray("coffee"), picture_or_gray("rocket"), picture_or_gray("cat"), color_bars(), corner_marks()};
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        if (!demo::write_picture_png(pictures[i], folder + "/shot" + std::to_string(i + 1) + ".png", 4)) return c.expect(false, "測試用的圖片檔寫不出來");
    }
    Rig rig(12);
    App& app = *rig.app;
    app.set_source_kind(SourceKind::images);
    OffscreenUI screen(app, 1280, 840, 1.0f);
    if (!screen.ok()) return c.expect(false, "離屏繪圖起不來：" + screen.error());
    const auto frames_until = [&](double timeout, const std::function<bool()>& condition) {
        const double deadline = uptime_seconds() + timeout;
        while (!condition()) {
            if (uptime_seconds() > deadline) return false;
            screen.frames(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    };
    const auto shows = [&](std::size_t index) { return app.source_preview != nullptr && demo::psnr(*app.source_preview, pictures[index]) > 45; };
    screen.frames(3);
    c.expect(!screen.ui().rect_of("##previmage").has_value() && !screen.ui().rect_of("##loopimages").has_value() && screen.ui().rect_of("##chooseimages").has_value(), "還沒選圖片時只有「選擇…」");
    c.expect(app.open_images({folder}, false) == 5, "選資料夾");
    c.expect(frames_until(10, [&] { return shows(0); }), "預覽應該是第一張");
    screen.frames(2);
    c.expect(screen.ui().rect_of("##previmage").has_value() && screen.ui().rect_of("##nextimage").has_value() && screen.ui().rect_of("##imageposition").has_value(), "選了圖片之後要有上一張、下一張與滑桿");
    c.expect(screen.click("##previmage") && app.image_index == 0, "已經是第一張時，「上一張」按了沒有作用");
    c.expect(screen.click("##nextimage") && app.image_index == 1 && frames_until(5, [&] { return shows(1); }), "按「下一張」：換到第二張，預覽跟著換");
    c.expect(screen.click("##nextimage") && screen.click("##nextimage") && app.image_index == 3, "再按兩次：第四張");
    c.expect(screen.click("##previmage") && app.image_index == 2 && frames_until(5, [&] { return shows(2); }), "按「上一張」：回到第三張");
    // 滑桿：按在最右邊是最後一張，最左邊是第一張，正中間是第三張
    if (const std::optional<UI::HitRect> bar = screen.ui().rect_of("##imageposition")) {
        screen.click(bar->x + bar->w - 1, bar->y + bar->h / 2);
        c.expect(app.image_index == 4 && frames_until(5, [&] { return shows(4); }), "滑桿拉到最右邊：最後一張（在第 " + std::to_string(app.image_index + 1) + " 張）");
        c.expect(screen.click("##nextimage") && app.image_index == 4, "已經是最後一張時，「下一張」按了沒有作用");
        screen.click(bar->x + 1, bar->y + bar->h / 2);
        c.expect(app.image_index == 0, "滑桿拉到最左邊：第一張（在第 " + std::to_string(app.image_index + 1) + " 張）");
        screen.click(bar->x + bar->w / 2, bar->y + bar->h / 2);
        c.expect(app.image_index == 2, "滑桿按在正中間：五張裡的第三張（在第 " + std::to_string(app.image_index + 1) + " 張）");
    }
    // 滑鼠停在檔名的框上：跳出一小塊說明，寫著下一張要送的圖片的完整路徑
    {
        screen.move_mouse(5, 5);
        screen.frames(3);
        const std::vector<std::uint8_t> away = screen.pixels();
        const std::optional<UI::HitRect> choose = screen.ui().rect_of("##chooseimages");
        if (choose) screen.move_mouse(choose->x - 120, choose->y + choose->h / 2);
        screen.frames(30);   // 說明要停一下才出現
        const std::vector<std::uint8_t> hovering = screen.pixels();
        std::size_t changed = 0;
        for (std::size_t i = 0; i + 2 < away.size() && i + 2 < hovering.size(); i += 3) changed += away[i] != hovering[i] || away[i + 1] != hovering[i + 1] || away[i + 2] != hovering[i + 2] ? 1 : 0;
        c.expect(choose && changed > 1500, "滑鼠停在圖片檔名的框上應該跳出說明（畫面上變了 " + std::to_string(changed) + " 個點）");
        keep_screen(screen, "images-name-tooltip");
        screen.move_mouse(5, 5);
        screen.frames(3);
    }
    c.expect(!app.settings.loop_images && screen.click("##loopimages") && app.settings.loop_images && app.settings.loop_video, "勾圖片序列的「循環播放」（和影片檔的是分開的兩個設定）");
    c.expect(screen.click("##loopimages") && !app.settings.loop_images, "再按一次取消");
    // 從按鈕發射：從第四張開始，送到第五張自己停
    app.select_image(3);
    c.expect(frames_until(5, [&] { return shows(3) && app.can_transmit(); }), "位置移到第四張");
    screen.frames(2);
    c.expect(screen.click("##transmit") && app.tx_phase != App::TxPhase::idle, "按「開始發射」");
    if (frames_until(20, [&] { return app.tx_phase == App::TxPhase::transmitting && app.tx_preview != nullptr; })) {
        screen.frames(2);
        keep_screen(screen, "images-transmitting");
    }
    c.expect(frames_until(40, [&] { return app.tx_phase == App::TxPhase::idle; }), "圖片送完要自己停止");
    c.expect(app.tx_frames_sent == 2 && app.image_index == 0, "從第四張開始應該送兩張（送出 " + std::to_string(app.tx_frames_sent) + " 張），位置回到第一張");
    c.expect(rig.wait(3, [&] { return rig.ptt->keys().size() == 2; }) && rig.ptt->keys() == std::vector<bool>({true, false}), "PTT 按下一次、放開一次");
    const std::vector<DecodedFrame> received = decode(rig.sink->take());
    c.expect(received.size() == 2 && received[0].picture && received[1].picture && demo::psnr(*received[0].picture, pictures[3]) > demo::psnr(*received[0].picture, pictures[4]) &&
                 demo::psnr(*received[1].picture, pictures[4]) > demo::psnr(*received[1].picture, pictures[3]),
             "送出去的應該是第四張、第五張");
}

void test_ui_transmit(Check& c) {
    Rig rig(12);
    rig.set_still(picture_or_gray("coffee"));
    OffscreenUI screen(*rig.app, 1280, 840, 1.0f);
    if (!screen.ok()) return c.expect(false, "離屏繪圖起不來：" + screen.error());
    const auto frames_until = [&](double timeout, const std::function<bool()>& condition) {
        const double deadline = uptime_seconds() + timeout;
        while (!condition()) {
            if (uptime_seconds() > deadline) return false;
            screen.frames(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    };
    c.expect(frames_until(5, [&] { return rig.app->can_transmit() && rig.app->source_preview != nullptr; }), "來源的預覽沒有出來");
    screen.frames(2);
    c.expect(screen.click("##transmit") && rig.app->tx_phase != App::TxPhase::idle, "按「開始發射」");
    c.expect(frames_until(40, [&] { return rig.app->tx_frames_sent >= 1; }), "應該送出一張");
    c.expect(!screen.ui().rect_of("##savevideo").has_value(), "發射中主畫面顯示的是送出的畫面，沒有存檔的按鈕");
    c.expect(screen.click("##transmit") && rig.app->tx_phase == App::TxPhase::stopping, "再按一次：送完這張就停");
    c.expect(frames_until(20, [&] { return rig.app->tx_phase == App::TxPhase::idle; }), "要回到待機");
    c.expect(rig.wait(3, [&] { return rig.ptt->keys().size() == 2; }) && rig.ptt->keys() == std::vector<bool>({true, false}), "PTT 按下一次、放開一次");
    c.expect(decode(rig.sink->take()).size() == static_cast<std::size_t>(rig.app->tx_frames_sent), "從介面按出來的發射，送出的每一張都解得出來");
}

void test_sdl_audio(Check& c) {
    // 只有在指定用 SDL 的「假」音訊驅動（SDL_AUDIODRIVER=dummy）時才跑：它不碰任何真的音效裝置，卻走和真的裝置一樣的程式路徑
    const char* driver = std::getenv("SDL_AUDIODRIVER");
    if (driver == nullptr || std::string(driver) != "dummy") return c.skip("沒有設定 SDL_AUDIODRIVER=dummy（自我測試不碰真的音效裝置）", "audio");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return c.expect(false, std::string("SDL 的音訊起不來：") + SDL_GetError());
    const char* current = SDL_GetCurrentAudioDriver();
    if (current == nullptr || std::string(current) != "dummy") {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return c.expect(false, std::string("音訊驅動不是 dummy，而是 ") + (current != nullptr ? current : "（沒有）"));
    }
    c.info("輸入裝置 " + std::to_string(audio_inputs().size()) + " 個，輸出裝置 " + std::to_string(audio_outputs().size()) + " 個");
    const std::optional<AudioDevice> playback = resolve_audio_device("", false), recording = resolve_audio_device("", true);
    c.expect(playback.has_value() && recording.has_value(), "系統預設的輸入、輸出裝置");
    c.expect(!resolve_audio_device("沒有這個裝置", false).has_value(), "指定的裝置不在時不能改用別的");
    if (playback && recording) {
        std::string error;
        AudioOutput output(4.0);
        c.expect(output.start(playback->id, &error), "輸出裝置開不起來：" + error);
        output.enqueue(std::vector<float>(24000, 0.1f));
        c.expect(demo::wait_until(nullptr, 5, [&] { return output.played_samples() >= 24000; }), "排進去的取樣沒有被裝置取走");
        const std::int64_t rendered = output.rendered_frames();
        c.expect(demo::wait_until(nullptr, 3, [&] { return output.rendered_frames() > rendered; }), "佇列空了裝置也要繼續來取（取到的是靜音）");
        output.stop();
        AudioInput input;
        std::mutex mutex;
        std::size_t received = 0;
        input.on_samples = [&](std::vector<float> samples) {
            std::lock_guard<std::mutex> lock(mutex);
            received += samples.size();
        };
        c.expect(input.start(recording->id, &error) && input.running(), "輸入裝置開不起來：" + error);
        c.expect(demo::wait_until(nullptr, 5, [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return received >= 24000;
        }), "輸入裝置沒有送取樣過來");
        input.stop();
    }
    // 整個 app 照平常的方式啟動（開始接收），再發射兩張
    {
        const std::shared_ptr<PttLog> log = std::make_shared<PttLog>();
        Settings settings = Settings::load("");
        settings.callsign = "BX4ACP";
        settings.tx_mode_id = 12;
        settings.debug_ptt_driver = [log] { return std::make_unique<RecordingPTT>(log, false, false); };
        App app(std::move(settings), true);
        app.start();
        c.expect(demo::wait_until(&app, 5, [&] { return app.input_level_db < 0 && !app.input_devices.empty(); }), "啟動後應該開始接收");
        c.expect(!app.notice.has_value(), "啟動後不應該有錯誤訊息：" + app.notice.value_or(""));
        auto frame = std::make_shared<SourceFrame>();
        frame->width = kImageWidth;
        frame->height = kImageHeight;
        const Picture coffee = picture_or_gray("coffee");
        frame->rgb.assign(coffee.rgb.begin(), coffee.rgb.end());
        app.debug_set_source_frame(frame);
        app.start_transmit();
        c.expect(demo::wait_until(&app, 40, [&] { return app.tx_frames_sent >= 2; }), "經過 SDL 的輸出裝置應該送得出兩張：" + app.notice.value_or(""));
        app.stop_transmit();
        c.expect(demo::wait_until(&app, 20, [&] { return app.tx_phase == App::TxPhase::idle; }), "要回到待機");
        c.expect(demo::wait_until(&app, 3, [&] { return log->keys().size() == 2; }) && log->keys() == std::vector<bool>({true, false}), "PTT 按下一次、放開一次");
        c.expect(!app.notice.has_value(), "正常結束不應該有訊息：" + app.notice.value_or(""));
        app.shutdown();
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

}  // namespace

int run(const Options& options) {
    // 暫存檔的目錄
    std::string work = options.work_directory;
    bool temporary = false;
    if (work.empty()) {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        work = (error ? std::filesystem::path(".") : base).string() + "/nbtv-gui-selftest-" + std::to_string(static_cast<long long>(schedule::now()));
        temporary = true;
    }
    SDL_CreateDirectory(work.c_str());
    i18n::use("zh-Hant");

    const std::vector<Case> cases = {
        {"i18n", "五種語言的翻譯", test_i18n},
        {"settings", "設定檔的讀寫", [&work](Check& c) { test_settings(c, work); }},
        {"schedule", "定時發射的時刻", test_schedule},
        {"models", "模型與模式", test_models},
        {"loopback", "每個模式的收發", test_loopback},
        {"channel", "有雜訊、有頻偏的收訊", test_channel},
        {"scale", "來源畫面的縮放", test_scale},
        {"png", "圖片檔的讀寫", [&work](Check& c) { test_png(c, work); }},
        {"fonts", "系統的字型", test_fonts},
        {"canvas", "畫布的字", test_canvas},
        {"waterfall", "瀑布圖", test_waterfall},
        {"audio-output", "輸出佇列（模擬的裝置）", test_audio_output},
        {"net", "TCP 連線", test_net},
        {"rigctld", "rigctld 的 PTT", test_rigctld},
        {"hamlib-parse", "Hamlib 的型號清單與參數", test_hamlib_parse},
        {"process", "啟動別的程式、看門的程序", [&options](Check& c) { test_process(c, options); }},
        {"hamlib-ptt", "用 Hamlib 的假電台按 PTT", test_hamlib_ptt},
        {"serial", "序列埠", test_serial},
        {"transmit", "發射的流程", test_transmit},
        {"transmit-canvas", "畫布的字跟著畫面送出去", test_transmit_canvas},
        {"transmit-abort", "立刻停止與取消", test_transmit_abort},
        {"transmit-failures", "PTT 失敗、輸出裝置不回應", test_transmit_failures},
        {"file-sources", "圖片與影片檔來源", [&work](Check& c) { test_file_sources(c, work); }},
        {"history", "最近收到、存圖片、存影片", [&work](Check& c) { test_history(c, work); }},
        {"sequence-stamp", "存成影片時在每一張的左上角印上訊雜比", [&work](Check& c) { test_video_stamp(c, work); }},
        {"schedule-run", "定時發射實際跑一次", [&work](Check& c) { test_schedule_run(c, work); }},
        {"images", "圖片序列：挑出圖片檔、照檔名排、照記載的方向轉正", [&work](Check& c) { test_image_files(c, work); }},
        {"images-transmit", "圖片序列：依序一張一張送、送完自動停止、沒送出去的下次再送", [&work](Check& c) { test_image_transmit(c, work); }},
        {"ui-render", "介面：各語言、各面板、各種視窗大小", [&work](Check& c) { test_ui_render(c, work); }},
        {"ui-click", "介面：按鈕、選單、輸入框", test_ui_click},
        {"ui-images", "介面：圖片序列的上一張、下一張、滑桿，從按鈕發射", [&work](Check& c) { test_ui_images(c, work); }},
        {"ui-transmit", "介面：從按鈕發射", test_ui_transmit},
        {"sdl-audio", "SDL 的音訊裝置（假的驅動）", test_sdl_audio},
    };

    int passed = 0, failed = 0, skipped = 0;
    bool matched = false;
    const double began = uptime_seconds();
    for (const Case& test : cases) {
        if (!options.only.empty() && options.only != test.name) continue;
        matched = true;
        Check check;
        const double start = uptime_seconds();
        test.body(check);
        MainQueue::shared().drain();
        const std::string label = std::string(test.name) + "（" + test.title + "）";
        const std::string seconds = number(uptime_seconds() - start, 1) + " 秒";
        if (!check.failures().empty()) {
            ++failed;
            log::line("[selftest] ✗ " + label + " " + seconds);
            for (const std::string& failure : check.failures()) log::line("[selftest]     失敗：" + failure);
        } else if (check.skipped()) {
            ++skipped;
            log::line("[selftest] － " + label + "：略過，" + *check.skipped());
        } else {
            ++passed;
            log::line("[selftest] ✓ " + label + " " + seconds);
        }
        for (const std::string& note : check.notes()) log::line("[selftest]     " + note);
    }
    if (!matched) {
        log::line("[selftest] 沒有叫做「" + options.only + "」的項目。有這些：");
        for (const Case& test : cases) log::line(std::string("[selftest]     ") + test.name + "　" + test.title);
        return 2;
    }
    if (temporary) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(work), error);
    }
    log::line("[selftest] 通過 " + std::to_string(passed) + " 項，失敗 " + std::to_string(failed) + " 項，略過 " + std::to_string(skipped) + " 項，共 " + number(uptime_seconds() - began, 1) + " 秒");
    return failed == 0 ? 0 : 1;
}

}  // namespace nbtvgui::selftest
