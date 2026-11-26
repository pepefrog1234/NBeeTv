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

namespace {

/// 權重檔的路徑 → 模型的名稱（檔名去掉副檔名），例如「261123-1」
std::string model_name_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0) name.resize(name.size() - 4);
    return name;
}

}  // namespace

// 哪個模式用哪一份權重檔，照命令列工具的那一份對照（tools/model_file.hpp）：核心模式表裡的每個模式各問一次，
// 同一份只載入一次（模式 5 和 15 共用一份）。所以載入的順序就是模式表的順序：模式 11、12、13、14、15 的
// （第三、四份是加寬的大型網路，第五份起的畫面是 192×144），接著是模式 6、7 的。和 macOS 版 CoreBridge.swift 載入的相同。
// 「合併最近幾幀」用的權重每個畫面模型各配一份（配哪一個記在檔案裡，由核心對應）；192×144 的大畫面沒有。
Models::Models() {
    std::vector<std::string> model_paths, merge_paths;
    const auto add = [](std::vector<std::string>& list, const std::string& path) {
        if (!path.empty() && std::find(list.begin(), list.end(), path) == list.end()) list.push_back(path);
    };
    nbtv_mode_info info{};
    for (int index = 0; index < nbtv_mode_count(); ++index) {
        if (nbtv_mode_at(index, &info) != 0) continue;
        add(model_paths, nbtv::tools::default_model_path(info.id));
        add(merge_paths, nbtv::tools::default_merge_path(info.id));
    }
    for (const std::string& path : model_paths) {
        std::vector<std::uint8_t> bytes;
        if (!nbtv::tools::read_file(path, bytes)) continue;
        if (nbtv_model* handle = nbtv_model_load(bytes.data(), bytes.size())) models_.push_back({model_name_of(path), handle});
    }
    for (const std::string& path : merge_paths) {
        std::vector<std::uint8_t> bytes;
        if (!nbtv::tools::read_file(path, bytes)) continue;
        if (nbtv_merge* handle = nbtv_merge_load(bytes.data(), bytes.size())) merges_.push_back({model_name_of(path), handle});
    }
}

const Models& Models::shared() {
    static const Models models;
    return models;
}

const LoadedModel* Models::find(int values, int width, PictureSize picture) const {
    // 實數個數與寬度都相同、畫面大小不同的模型是給別的模式用的（模式 11 與模式 15 就是這樣）
    for (const LoadedModel& model : models_) {
        if (nbtv_model_values(model.handle) == values && nbtv_model_width(model.handle) == width &&
            nbtv_model_image_width(model.handle) == picture.width && nbtv_model_image_height(model.handle) == picture.height)
            return &model;
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

std::string ModeInfo::detail() const { return pilot_free() ? L("FM 無導頻") : large_picture() ? L("FM 大畫面") : quality(); }

std::string ModeInfo::menu_detail() const {
    // 沒有導頻的模式在選單裡自成一組（標題已經說了是哪一種幀），這裡只寫畫面的細緻程度：一張畫面送的值愈多愈細
    // （模式 3、4 最少，是「快速」；模式 5 和模式 15 一樣多，是「標準」；模式 6 是「較細」；模式 7 是「細緻」）。
    if (!pilot_free()) return detail();
    return learned_values <= 4000 ? L("快速") : learned_values <= 8000 ? L("標準") : learned_values <= 12000 ? L("較細") : L("細緻");
}

bool ModeInfo::preview_apart() const {
    const double wide = learned_width / 32.0, large = static_cast<double>(picture.rgb_bytes()) / kStandardPicture.rgb_bytes();
    return 0.054 * wide * wide * large > 0.5 * frame_seconds;
}

std::string ModeInfo::label() const { return name() + " · " + detail(); }

std::string ModeInfo::seconds_text() const { return LF("%.2f 秒", {frame_seconds}); }

std::string ModeInfo::content_text() const { return large_picture() ? picture.text() : large_model ? L("大型網路") : L("類神經網路"); }

std::string ModeInfo::summary() const {
    const std::string text = large_picture() && pilot_free() ? LF("每張 %@ · %@ 的大畫面，幀裡沒有導頻，只給 FM 用，實驗中", {seconds_text(), picture.text()})
                             : pilot_free()                  ? LF("每張 %@ · 幀裡沒有導頻，只給 FM 用，實驗中", {seconds_text()})
                             : large_picture()               ? LF("每張 %@ · %@ 的大畫面，給 FM 用，實驗中", {seconds_text(), picture.text()})
                             : large_model                   ? LF("每張 %@ · 大型的類神經網路編碼，實驗中", {seconds_text()})
                                                             : LF("每張 %@ · 類神經網路編碼，實驗中", {seconds_text()});
    return model_name.empty() ? text : text + LF(" · 模型 %@", {model_name});
}

namespace modes {

const std::vector<ModeInfo>& all() {
    static const std::vector<ModeInfo> list = [] {
        std::vector<ModeInfo> found;
        nbtv_mode_info info{};
        for (int index = 0; index < nbtv_mode_count(); ++index) {
            if (nbtv_mode_at(index, &info) != 0) continue;
            const PictureSize picture{info.image_width, info.image_height};
            const LoadedModel* model = Models::shared().find(info.learned_values, info.learned_width, picture);
            if (model == nullptr) continue;
            ModeInfo mode;
            mode.id = info.id;
            mode.frame_seconds = info.frame_seconds;
            mode.frame_symbols = info.frame_symbols;
            mode.payload_symbols = info.payload_symbols;
            mode.learned_values = info.learned_values;
            mode.learned_width = info.learned_width;
            mode.picture = picture;
            mode.scattered_pilots = info.scattered_pilots != 0;
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
        const bool a_free = a.pilot_free(), b_free = b.pilot_free();
        return std::tie(a.picture.width, a_free, a.payload_symbols, a.learned_width, a.id) < std::tie(b.picture.width, b_free, b.payload_symbols, b.learned_width, b.id);
    });
    return list;
}

MenuGroups menu_groups() {
    MenuGroups groups;
    for (const ModeInfo& mode : menu_order()) {
        if (!mode.large_picture()) (mode.pilot_free() ? groups.quick : groups.standard).push_back(mode);
        else if (mode.pilot_free()) groups.pilot_free.push_back(mode);
        else groups.large.push_back(mode);
    }
    return groups;
}

std::vector<int> without_model() {
    std::vector<int> missing;
    nbtv_mode_info info{};
    for (int index = 0; index < nbtv_mode_count(); ++index) {
        if (nbtv_mode_at(index, &info) == 0 && find(info.id) == nullptr) missing.push_back(info.id);
    }
    return missing;
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

void CoreReceiver::set_decode_pictures(bool on) { nbtv_rx_set_decode_pictures(handle_, on ? 1 : 0); }

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
    // 畫面的大小跟著模式（核心填在這一幀裡）；緩衝區是照最大的畫面配的，超出它的不拿（不應該發生）
    const PictureSize size{frame_->image_width, frame_->image_height};
    const bool fits = size.width > 0 && size.height > 0 && size.width <= NBTV_IMAGE_MAX_WIDTH && size.height <= NBTV_IMAGE_MAX_HEIGHT;
    frame.decoded = frame_->decoded != 0 && fits;
    frame.model_missing = frame_->model_missing != 0;
    frame.skipped = frame_->skipped != 0;
    frame.header_ok = frame_->header_ok != 0;
    frame_->callsign[NBTV_CALLSIGN_MAX] = '\0';
    frame.callsign = frame_->callsign;
    frame.sequence = frame_->sequence;
    frame.snr_db = frame_->snr_db;
    frame.offset_hz = frame_->freq_offset_hz;
    if (frame.decoded) {
        auto picture = std::make_shared<Picture>(size);
        std::memcpy(picture->rgb.data(), frame_->rgb, picture->rgb.size());
        frame.picture = picture;
    }
    return frame;
}

CoreTransmitter::CoreTransmitter(int sample_rate) : handle_(nbtv_tx_create(sample_rate)) {
    for (const LoadedModel& model : Models::shared().models()) nbtv_tx_set_model(handle_, model.handle);
    // 一開始是核心預設的模式（模式表的第一個）
    nbtv_mode_info info{};
    if (nbtv_mode_by_id(nbtv_tx_mode(handle_), &info) == 0) picture_size_ = PictureSize{info.image_width, info.image_height};
}

CoreTransmitter::~CoreTransmitter() { nbtv_tx_destroy(handle_); }

bool CoreTransmitter::set_mode(int id) {
    nbtv_mode_info info{};
    if (nbtv_mode_by_id(id, &info) != 0 || nbtv_tx_set_mode(handle_, id) != 0) return false;
    picture_size_ = PictureSize{info.image_width, info.image_height};
    return true;
}

void CoreTransmitter::set_callsign(const std::string& callsign) { nbtv_tx_set_callsign(handle_, callsign.c_str()); }

int CoreTransmitter::frame_samples() const { return nbtv_tx_frame_samples(handle_); }

std::optional<EncodedFrame> CoreTransmitter::encode(const Picture& picture, bool preview_apart) {
    // 核心照目前這個模式的大小讀 rgb：大小不對的畫面不能交給它
    if (picture.size != picture_size_ || picture.rgb.size() != static_cast<std::size_t>(picture_size_.rgb_bytes())) return std::nullopt;
    const int count = frame_samples();
    EncodedFrame frame;
    frame.samples.resize(static_cast<std::size_t>(count));
    auto preview = preview_apart ? nullptr : std::make_shared<Picture>(picture_size_);
    nbtv_tx_frame_info info{};
    const int written =
        nbtv_tx_encode_image(handle_, picture.rgb.data(), frame.samples.data(), count, preview ? preview->rgb.data() : nullptr, &info);
    if (written != count) return std::nullopt;
    frame.preview = preview;
    frame.mode_id = info.mode_id;
    frame.sequence = info.sequence;
    if (preview_apart) {
        nbtv_mode_info mode{};
        if (nbtv_mode_by_id(info.mode_id, &mode) != 0) return std::nullopt;
        frame.values.resize(static_cast<std::size_t>(mode.learned_values));
        if (nbtv_tx_last_values(handle_, frame.values.data(), mode.learned_values) != mode.learned_values) return std::nullopt;
    }
    return frame;
}

PicturePtr preview_from_values(const std::vector<float>& values, int mode_id) {
    const ModeInfo* mode = modes::find(mode_id);
    if (mode == nullptr || static_cast<int>(values.size()) != mode->learned_values) return nullptr;
    const LoadedModel* model = Models::shared().find(mode->learned_values, mode->learned_width, mode->picture);
    if (model == nullptr) return nullptr;
    auto picture = std::make_shared<Picture>(mode->picture);
    if (nbtv_model_decode(model->handle, values.data(), static_cast<int>(values.size()), picture->rgb.data()) != 0) return nullptr;
    return picture;
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
