#include "core_bridge.hpp"

#include <algorithm>
#include <cstring>
#include <tuple>

#include "i18n.hpp"
#include "model_file.hpp"

namespace nbtvgui {

std::string normalize_callsign(const std::string& text) {
    char buffer[NBTV_CALLSIGN_MAX + 8] = {};
    // 核心只看到第一個結尾的 0 為止；輸入再長也只會寫出最多 9 個字元
    nbtv_callsign_normalize(text.c_str(), buffer);
    return buffer;
}

// 依序是模式 11、12、13、14 用的模型；後兩份是加寬的大型網路。和 tools/model_file.hpp、macOS 版的 CoreBridge.swift 相同。
static const char* const kModelNames[] = {"261123-1", "261122-3", "261122-4", "261123-4"};
// 接收端「合併最近幾幀」用的權重，每個畫面模型各配一份（配哪一個記在檔案裡，由核心對應）。
static const char* const kMergeNames[] = {"261124-10", "261124-8", "261124-9", "261124-7"};

Models::Models() {
    for (const char* name : kModelNames) {
        std::vector<std::uint8_t> bytes;
        if (!nbtv::tools::read_file(nbtv::tools::model_file_path(name), bytes)) continue;
        if (nbtv_model* handle = nbtv_model_load(bytes.data(), bytes.size())) models_.push_back({name, handle});
    }
    for (const char* name : kMergeNames) {
        std::vector<std::uint8_t> bytes;
        if (!nbtv::tools::read_file(nbtv::tools::model_file_path(name), bytes)) continue;
        if (nbtv_merge* handle = nbtv_merge_load(bytes.data(), bytes.size())) merges_.push_back({name, handle});
    }
}

const Models& Models::shared() {
    static const Models models;
    return models;
}

const LoadedModel* Models::find(int values, int width) const {
    for (const LoadedModel& model : models_) {
        if (nbtv_model_values(model.handle) == values && nbtv_model_width(model.handle) == width) return &model;
    }
    return nullptr;
}

bool Models::merge_covers(std::uint32_t model_id) const {
    for (const LoadedMerge& merge : merges_) {
        if (nbtv_merge_model_id(merge.handle) == model_id) return true;
    }
    return false;
}

std::string ModeInfo::name() const { return LF("模式 %d", {id}); }

std::string ModeInfo::quality() const {
    // 依每幀的酬載符號數取的名字：愈長的幀放得下愈多的值，畫面愈細，但每張也愈久。
    // 目前的模式是 23（快速）、37（粗略）、100（中等）個符號。
    return payload_symbols < 30 ? L("快速") : payload_symbols < 60 ? L("粗略") : payload_symbols < 150 ? L("中等") : L("細緻");
}

std::string ModeInfo::label() const { return name() + " · " + quality(); }

std::string ModeInfo::seconds_text() const { return LF("%.2f 秒", {frame_seconds}); }

std::string ModeInfo::content_text() const { return large_model ? L("大型網路") : L("類神經網路"); }

std::string ModeInfo::summary() const {
    const std::string text = large_model ? LF("每張 %@ · 大型的類神經網路編碼，實驗中", {seconds_text()}) : LF("每張 %@ · 類神經網路編碼，實驗中", {seconds_text()});
    return model_name.empty() ? text : text + LF(" · 模型 %@", {model_name});
}

namespace modes {

const std::vector<ModeInfo>& all() {
    static const std::vector<ModeInfo> list = [] {
        std::vector<ModeInfo> found;
        nbtv_mode_info info{};
        for (int index = 0; index < nbtv_mode_count(); ++index) {
            if (nbtv_mode_at(index, &info) != 0) continue;
            const LoadedModel* model = Models::shared().find(info.learned_values, info.learned_width);
            if (model == nullptr) continue;
            ModeInfo mode;
            mode.id = info.id;
            mode.frame_seconds = info.frame_seconds;
            mode.frame_symbols = info.frame_symbols;
            mode.payload_symbols = info.payload_symbols;
            mode.learned_values = info.learned_values;
            mode.learned_width = info.learned_width;
            mode.model_name = model->name;
            found.push_back(mode);
        }
        // 模型比其他模式寬的標成「大型」（模式 13、14）
        int narrowest = 0;
        for (const ModeInfo& mode : found) narrowest = narrowest == 0 ? mode.learned_width : std::min(narrowest, mode.learned_width);
        for (ModeInfo& mode : found) mode.large_model = mode.learned_width > narrowest;
        return found;
    }();
    return list;
}

std::vector<ModeInfo> menu_order() {
    std::vector<ModeInfo> list = all();
    std::sort(list.begin(), list.end(), [](const ModeInfo& a, const ModeInfo& b) {
        return std::tie(a.payload_symbols, a.learned_width, a.id) < std::tie(b.payload_symbols, b.learned_width, b.id);
    });
    return list;
}

const ModeInfo* find(int id) {
    for (const ModeInfo& mode : all()) {
        if (mode.id == id) return &mode;
    }
    return nullptr;
}

int default_id() {
    if (find(11) != nullptr) return 11;
    return all().empty() ? 11 : all().front().id;
}

}  // namespace modes

CoreReceiver::CoreReceiver(int sample_rate) : handle_(nbtv_rx_create(sample_rate)), frame_(std::make_unique<nbtv_rx_frame>()) {
    for (const LoadedModel& model : Models::shared().models()) nbtv_rx_set_model(handle_, model.handle);
    for (const LoadedMerge& merge : Models::shared().merges()) nbtv_rx_set_merge(handle_, merge.handle);
}

CoreReceiver::~CoreReceiver() { nbtv_rx_destroy(handle_); }

void CoreReceiver::reset() { nbtv_rx_reset(handle_); }

void CoreReceiver::set_merging(bool on) { nbtv_rx_set_merging(handle_, on ? 1 : 0); }

void CoreReceiver::push(const float* samples, int count) { nbtv_rx_push(handle_, samples, count); }

nbtv_rx_status CoreReceiver::status() const {
    nbtv_rx_status status{};
    nbtv_rx_get_status(handle_, &status);
    return status;
}

std::optional<DecodedFrame> CoreReceiver::poll_frame() {
    if (nbtv_rx_poll_frame(handle_, frame_.get()) != 1) return std::nullopt;
    DecodedFrame frame;
    frame.mode_id = frame_->mode_id;
    frame.decoded = frame_->decoded != 0;
    frame.model_missing = frame_->model_missing != 0;
    frame.header_ok = frame_->header_ok != 0;
    frame_->callsign[NBTV_CALLSIGN_MAX] = '\0';
    frame.callsign = frame_->callsign;
    frame.sequence = frame_->sequence;
    frame.snr_db = frame_->snr_db;
    frame.offset_hz = frame_->freq_offset_hz;
    if (frame.decoded) {
        auto picture = std::make_shared<Picture>();
        std::memcpy(picture->rgb.data(), frame_->rgb, kRgbBytes);
        frame.picture = picture;
    }
    return frame;
}

CoreTransmitter::CoreTransmitter(int sample_rate) : handle_(nbtv_tx_create(sample_rate)) {
    for (const LoadedModel& model : Models::shared().models()) nbtv_tx_set_model(handle_, model.handle);
}

CoreTransmitter::~CoreTransmitter() { nbtv_tx_destroy(handle_); }

bool CoreTransmitter::set_mode(int id) { return nbtv_tx_set_mode(handle_, id) == 0; }

void CoreTransmitter::set_callsign(const std::string& callsign) { nbtv_tx_set_callsign(handle_, callsign.c_str()); }

int CoreTransmitter::frame_samples() const { return nbtv_tx_frame_samples(handle_); }

std::optional<EncodedFrame> CoreTransmitter::encode(const Picture& picture) {
    const int count = frame_samples();
    EncodedFrame frame;
    frame.samples.resize(static_cast<std::size_t>(count));
    auto preview = std::make_shared<Picture>();
    nbtv_tx_frame_info info{};
    const int written = nbtv_tx_encode_image(handle_, picture.rgb.data(), frame.samples.data(), count, preview->rgb.data(), &info);
    if (written != count) return std::nullopt;
    frame.preview = preview;
    frame.mode_id = info.mode_id;
    frame.sequence = info.sequence;
    return frame;
}

bool CoreTransmitter::discard_last() { return nbtv_tx_discard_last(handle_) == 0; }

std::vector<float> CoreTransmitter::finish() {
    std::vector<float> tail(256);
    const int n = nbtv_tx_finish(handle_, tail.data(), static_cast<int>(tail.size()));
    tail.resize(static_cast<std::size_t>(std::max(n, 0)));
    return tail;
}

CoreSpectrum::CoreSpectrum(int sample_rate) : handle_(nbtv_spectrum_create(sample_rate)) {}

CoreSpectrum::~CoreSpectrum() { nbtv_spectrum_destroy(handle_); }

void CoreSpectrum::reset() { nbtv_spectrum_reset(handle_); }

void CoreSpectrum::push(const float* samples, int count) { nbtv_spectrum_push(handle_, samples, count); }

bool CoreSpectrum::poll(std::array<float, kSpectrumBins>& row) { return nbtv_spectrum_poll(handle_, row.data()) == 1; }

}  // namespace nbtvgui
