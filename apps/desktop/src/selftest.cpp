#include "selftest.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

// ---------------------------------------------------------------- 測試用的 PTT 與輸出

struct PttLog {
    std::mutex mutex;
    std::vector<std::pair<bool, double>> events;   // 按下（true）或放開、發生的時刻
    std::vector<bool> keys() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<bool> result;
        for (const auto& event : events) result.push_back(event.first);
        return result;
    }
};

/// 不碰任何硬體、只把每一次按下與放開記下來的 PTT。fail_press：按下時回報失敗（uncertain：而且說不準電台有沒有按下）。
class RecordingPTT : public PTTDriver {
public:
    RecordingPTT(std::shared_ptr<PttLog> log, bool fail_press, bool uncertain) : log_(std::move(log)), fail_press_(fail_press), uncertain_(uncertain) {}
    PTTResult key(bool on) override {
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
    App::still_schedule_seconds = original;
}

void test_ui_render(Check& c) {
    const std::string original = i18n::language();
    const std::unique_ptr<App> receiving = snapshot::make_receiving_app(true);
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
        for (App* app : {transmitting.get(), idle.get(), receiving.get()}) {
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
        {"schedule-run", "定時發射實際跑一次", [&work](Check& c) { test_schedule_run(c, work); }},
        {"ui-render", "介面：各語言、各面板、各種視窗大小", test_ui_render},
        {"ui-click", "介面：按鈕、選單、輸入框", test_ui_click},
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
