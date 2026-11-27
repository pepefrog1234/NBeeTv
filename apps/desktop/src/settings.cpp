#include "settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>

#include "i18n.hpp"
#include "model_file.hpp"

namespace nbtvgui {

namespace {

std::string trim(const std::string& text) {
    std::size_t first = 0, last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n')) ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r' || text[last - 1] == '\n')) --last;
    return text.substr(first, last - first);
}

/// 值裡面的換行存檔前換成空白（一行一項）
std::string one_line(std::string text) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

std::tm local_time(double time) {
    const std::time_t seconds = static_cast<std::time_t>(std::floor(time));
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    return parts;
}

}  // namespace

std::string Settings::serialized() const {
    std::string out;
    const auto put = [&out](const char* key, const std::string& value) { out += std::string(key) + "=" + one_line(value) + "\n"; };
    char number[64];
    put("callsign", callsign);
    put("inputDevice", input_device);
    put("outputDevice", output_device);
    std::snprintf(number, sizeof number, "%.1f", output_level_db);
    put("outputLevelDb", number);
    put("pttMode", to_string(ptt_mode));
    put("serialPort", serial_port);
    put("serialLine", serial_line == SerialLine::rts ? "rts" : "dtr");
    put("rigHost", rig_host);
    put("rigPort", rig_port);
    put("rigModel", std::to_string(rig_model));
    put("rigModelName", rig_model_name);
    put("serialBaud", std::to_string(serial_baud));
    put("txModeID", std::to_string(tx_mode_id));
    put("sourceKind", source_kind == SourceKind::camera ? "camera" : source_kind == SourceKind::images ? "images" : "video");
    put("scaleMode", scale_mode == ScaleMode::fill ? "fill" : "fit");
    put("cameraName", camera_name);
    put("loopVideo", loop_video ? "1" : "0");
    put("videoPath", video_path);
    put("loopImages", loop_images ? "1" : "0");
    // 圖片序列的那一批路徑：一行一個，imagePath1、imagePath2…
    for (std::size_t i = 0; i < image_paths.size(); ++i) put(("imagePath" + std::to_string(i + 1)).c_str(), image_paths[i]);
    put("scheduleIntervalMinutes", std::to_string(schedule_interval_minutes));
    put("scheduleSkipWhenBusy", schedule_skip_when_busy ? "1" : "0");
    put("showFrameSNR", show_frame_snr ? "1" : "0");
    put("stampVideoSNR", stamp_video_snr ? "1" : "0");
    put("canvasEnabled", canvas_enabled ? "1" : "0");
    put("canvasText", canvas_text);
    put("canvasPosition", canvas_position == CanvasPosition::top ? "top" : "bottom");
    put("voiceEnabled", voice_enabled ? "1" : "0");
    put("voiceLayout", voice_layout == VoiceLayout::overlay ? "overlay" : voice_layout == VoiceLayout::narrow ? "narrow" : "standard");
    put("microphone", microphone);
    put("speaker", speaker);
    put("language", language);
    put("windowWidth", std::to_string(window_width));
    put("windowHeight", std::to_string(window_height));
    return out;
}

Settings Settings::load(const std::string& path) {
    Settings s;
    s.path_ = path;
    s.tx_mode_id = modes::default_id();
    std::map<std::string, std::string> values;
    std::vector<std::uint8_t> bytes;
    if (!path.empty() && nbtv::tools::read_file(path, bytes)) {
        const std::string text(bytes.begin(), bytes.end());
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(start, end - start);
            if (const std::size_t equals = line.find('='); equals != std::string::npos) values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
            start = end + 1;
        }
    }
    const auto text = [&values](const char* key, const std::string& fallback) {
        const auto found = values.find(key);
        return found == values.end() ? fallback : found->second;
    };
    const auto number = [&values](const char* key, double fallback) {
        const auto found = values.find(key);
        return found == values.end() || found->second.empty() ? fallback : std::atof(found->second.c_str());
    };
    const auto flag = [&values](const char* key, bool fallback) {
        const auto found = values.find(key);
        return found == values.end() || found->second.empty() ? fallback : found->second != "0";
    };
    s.callsign = normalize_callsign(text("callsign", ""));
    s.input_device = text("inputDevice", "");
    s.output_device = text("outputDevice", "");
    s.output_level_db = std::clamp(number("outputLevelDb", -6), -40.0, 0.0);
    s.ptt_mode = ptt_mode_from(text("pttMode", "none"));
    s.serial_port = text("serialPort", "");
    s.serial_line = text("serialLine", "rts") == "dtr" ? SerialLine::dtr : SerialLine::rts;
    s.rig_host = text("rigHost", "127.0.0.1");
    s.rig_port = text("rigPort", "4532");
    s.rig_model = std::max(0, static_cast<int>(number("rigModel", 0)));
    s.rig_model_name = text("rigModelName", "");
    const int baud = static_cast<int>(number("serialBaud", 0));
    s.serial_baud = std::find(hamlib::baud_rates().begin(), hamlib::baud_rates().end(), baud) != hamlib::baud_rates().end() ? baud : 0;
    const int mode = static_cast<int>(number("txModeID", modes::default_id()));
    s.tx_mode_id = modes::find(mode) != nullptr ? mode : modes::default_id();
    const std::string source = text("sourceKind", "video");
    s.source_kind = source == "camera" ? SourceKind::camera : source == "images" ? SourceKind::images : SourceKind::video;
    s.scale_mode = text("scaleMode", "fill") == "fit" ? ScaleMode::fit : ScaleMode::fill;
    s.camera_name = text("cameraName", "");
    s.loop_video = flag("loopVideo", true);
    s.video_path = text("videoPath", "");
    s.loop_images = flag("loopImages", false);
    // imagePath1、imagePath2…：數到沒有的那一個為止
    for (std::size_t i = 1;; ++i) {
        const auto found = values.find("imagePath" + std::to_string(i));
        if (found == values.end() || found->second.empty()) break;
        s.image_paths.push_back(found->second);
    }
    const int interval = static_cast<int>(number("scheduleIntervalMinutes", schedule::kDefaultInterval));
    s.schedule_interval_minutes = std::find(schedule::intervals().begin(), schedule::intervals().end(), interval) != schedule::intervals().end() ? interval : schedule::kDefaultInterval;
    s.schedule_skip_when_busy = flag("scheduleSkipWhenBusy", true);
    s.show_frame_snr = flag("showFrameSNR", false);
    s.stamp_video_snr = flag("stampVideoSNR", false);
    s.canvas_enabled = flag("canvasEnabled", false);
    s.canvas_text = text("canvasText", "");
    s.canvas_position = text("canvasPosition", "top") == "bottom" ? CanvasPosition::bottom : CanvasPosition::top;
    s.voice_enabled = flag("voiceEnabled", false);
    const std::string voice_layout = text("voiceLayout", "standard");
    s.voice_layout = voice_layout == "overlay" ? VoiceLayout::overlay : voice_layout == "narrow" ? VoiceLayout::narrow : VoiceLayout::standard;
    s.microphone = text("microphone", "");
    s.speaker = text("speaker", "");
    const std::string language = text("language", "");
    s.language = std::find(i18n::languages().begin(), i18n::languages().end(), language) != i18n::languages().end() ? language : "";
    s.window_width = std::max(0, static_cast<int>(number("windowWidth", 0)));
    s.window_height = std::max(0, static_cast<int>(number("windowHeight", 0)));
    s.saved_ = s.serialized();
    return s;
}

void Settings::save() {
    if (path_.empty()) return;
    const std::string text = serialized();
    if (text == saved_) return;
    if (std::FILE* f = std::fopen(path_.c_str(), "wb")) {
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        if (ok) saved_ = text;
    }
}

std::string Settings::ptt_summary() const {
    switch (ptt_mode) {
    case PTTMode::none: return L("不控制（VOX）");
    case PTTMode::serial: return LF("序列埠 %@", {title(serial_line)});
    case PTTMode::cat: return rig_model > 0 ? "CAT " + rig_model_title() : L("CAT（還沒選電台型號）");
    case PTTMode::rigctld: break;
    }
    return "rigctld " + rig_host + ":" + rig_port;
}

std::string Settings::rig_model_title() const {
    if (rig_model <= 0) return {};
    return rig_model_name.empty() ? LF("型號 %d", {rig_model}) : rig_model_name;
}

std::optional<CanvasStyle> Settings::canvas_style() const {
    const std::string text = trim(canvas_text);
    if (!canvas_enabled || text.empty()) return std::nullopt;
    return CanvasStyle{text, canvas_position};
}

std::unique_ptr<PTTDriver> Settings::make_ptt_driver() const {
    if (debug_ptt_driver) return debug_ptt_driver();
    switch (ptt_mode) {
    case PTTMode::none: return std::make_unique<NoPTT>();
    case PTTMode::serial: return std::make_unique<SerialPTT>(serial_port, serial_line);
    case PTTMode::cat: return std::make_unique<HamlibPTT>(rig_model, serial_port, serial_baud);
    case PTTMode::rigctld: break;
    }
    return std::make_unique<RigctldPTT>(rig_host, rig_port);
}

float Settings::output_gain() const { return static_cast<float>(std::pow(10.0, output_level_db / 20.0)); }

namespace schedule {

const std::vector<int>& intervals() {
    static const std::vector<int> list = {5, 10, 15, 20, 30, 60};
    return list;
}

double next_fire(double time, int interval_minutes) {
    const double step = static_cast<double>(std::max(interval_minutes, 1) * 60);
    // 從「這個小時的開頭」往後數，不是從某個固定的起點數：時區和世界協調時間差的不是整點的地方（例如印度的 +5:30），整點的位置才對
    const std::tm parts = local_time(time);
    const double into_hour = parts.tm_min * 60.0 + parts.tm_sec + (time - std::floor(time));
    const double hour_start = time - into_hour;
    const double slot = std::floor(into_hour / step) + 1;
    return std::min(hour_start + slot * step, hour_start + 3600.0);
}

std::string minutes_text(int interval_minutes) {
    std::vector<std::string> minutes;
    for (int minute = 0; minute < 60; minute += std::max(interval_minutes, 1)) {
        char text[8];
        std::snprintf(text, sizeof text, "%02d", minute);
        minutes.emplace_back(text);
    }
    const std::string separator = L("、");   // 列舉的分隔：中文、日文是頓號，英文、西班牙文是逗號加空白
    const auto join = [&](std::size_t count) {
        std::string joined;
        for (std::size_t i = 0; i < count; ++i) joined += (i > 0 ? separator : std::string()) + minutes[i];
        return joined;
    };
    if (minutes.size() <= 6) return join(minutes.size());
    return join(3) + " … " + minutes.back();
}

std::string clock_text(double time) {
    const std::tm parts = local_time(time);
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d", parts.tm_hour, parts.tm_min);
    return text;
}

std::string time_text(double time) {
    const std::tm parts = local_time(time);
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d", parts.tm_hour, parts.tm_min, parts.tm_sec);
    return text;
}

std::string stamp_text(double time) {
    const std::tm parts = local_time(time);
    char text[32];
    std::snprintf(text, sizeof text, "%04d%02d%02d-%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    return text;
}

double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

}  // namespace schedule

}  // namespace nbtvgui
