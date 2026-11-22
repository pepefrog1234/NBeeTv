#include "learned_frame.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <numeric>

#include "rng.hpp"

namespace nbtv {

namespace {

// 0..n−1 的亂數排列：Fisher–Yates 洗牌，亂數是核心自己的 Rng（各平台算出來的相同）。種子是 "LRN1"。
std::vector<std::uint16_t> make_order(int n) {
    std::vector<std::uint16_t> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), std::uint16_t{0});
    Rng rng(0x4C524E31ULL);
    for (std::uint32_t k = static_cast<std::uint32_t>(n) - 1; k > 0; --k) std::swap(order[k], order[rng.below(k + 1)]);
    return order;
}

}  // namespace

const std::vector<std::uint16_t>& learned_value_order(const Mode& mode) {
    static const std::array<std::vector<std::uint16_t>, kModes.size()> tables = [] {
        std::array<std::vector<std::uint16_t>, kModes.size()> t;
        for (std::size_t i = 0; i < kModes.size(); ++i) {
            if (kModes[i].is_learned()) t[i] = make_order(kModes[i].learned_values);
        }
        return t;
    }();
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (kModes[i].id == mode.id) return tables[i];
    }
    assert(false && "不是模式表裡的模式");
    return tables[0];
}

bool learned_model_fits(const Mode& mode, const LearnedCodec& codec) {
    return mode.is_learned() && codec.value_count() == mode.learned_values && codec.base_width() == mode.learned_width;
}

void LearnedModels::set(std::shared_ptr<const LearnedCodec> model) {
    if (model == nullptr) {
        models_.clear();
        return;
    }
    for (std::shared_ptr<const LearnedCodec>& held : models_) {
        if (held->value_count() == model->value_count() && held->base_width() == model->base_width()) {
            held = std::move(model);
            return;
        }
    }
    models_.push_back(std::move(model));
}

const LearnedCodec* LearnedModels::find(const Mode& mode) const {
    for (const std::shared_ptr<const LearnedCodec>& held : models_) {
        if (learned_model_fits(mode, *held)) return held.get();
    }
    return nullptr;
}

std::vector<cf> learned_encode(const Mode& mode, const LearnedCodec& codec, const AnalogHeader& header,
                               std::span<const std::uint8_t> rgb, std::vector<float>* values) {
    if (!learned_model_fits(mode, codec)) return {};
    std::vector<float> v(static_cast<std::size_t>(mode.learned_values));
    if (!codec.encode(rgb, v)) return {};

    std::vector<cf> plain(static_cast<std::size_t>(mode.data_cells()));  // 用不到的格點留 0
    AnalogHeader h = header;
    h.format = kLearnedFrameFormat;
    h.gain_code = static_cast<int>(codec.model_id() & 0xFFu);
    analog_header_encode(h, std::span<cf>(plain.data(), static_cast<std::size_t>(kAnalogHeaderCells)));
    // 編碼器的輸出已經是每個實數均方值 1/2，兩個一組就是平均功率 1 的格點，和數位模式的資料格相同。
    // 放進去之前先打散（原因見標頭檔）。
    const std::vector<std::uint16_t>& order = learned_value_order(mode);
    for (std::size_t k = 0; k < v.size(); k += 2) {
        plain[static_cast<std::size_t>(kAnalogHeaderCells) + k / 2] = cf(v[order[k]], v[order[k + 1]]);
    }

    std::vector<cf> cells(plain.size());
    for (int i = 0; i < mode.data_cells(); ++i) cells[static_cast<std::size_t>(mode.interleaved_position(i))] = plain[static_cast<std::size_t>(i)];
    if (values != nullptr) *values = std::move(v);
    return cells;
}

LearnedUnpacked learned_unpack(const Mode& mode, std::span<const cf> cells, std::span<const float> noise_var) {
    assert(mode.is_learned());
    assert(static_cast<int>(cells.size()) == mode.data_cells() && noise_var.size() == cells.size());

    std::vector<cf> plain(cells.size());
    std::vector<float> var(cells.size());
    for (int i = 0; i < mode.data_cells(); ++i) {
        const std::size_t pos = static_cast<std::size_t>(mode.interleaved_position(i));
        plain[static_cast<std::size_t>(i)] = cells[pos];
        var[static_cast<std::size_t>(i)] = std::max(noise_var[pos], 1e-12f);
    }

    LearnedUnpacked out;
    out.header_ok = analog_header_decode(std::span<const cf>(plain.data(), static_cast<std::size_t>(kAnalogHeaderCells)),
                                         std::span<const float>(var.data(), static_cast<std::size_t>(kAnalogHeaderCells)), out.header);
    const std::size_t n = static_cast<std::size_t>(mode.learned_values);
    out.values.resize(n);
    out.noise_var.resize(n);
    const std::vector<std::uint16_t>& order = learned_value_order(mode);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t cell = static_cast<std::size_t>(kAnalogHeaderCells) + k / 2;
        out.values[order[k]] = (k % 2 == 0) ? plain[cell].real() : plain[cell].imag();
        out.noise_var[order[k]] = var[cell];
    }
    return out;
}

bool learned_model_matches(const LearnedUnpacked& frame, const LearnedCodec& codec) {
    if (!frame.header_ok) return true;
    return frame.header.format == kLearnedFrameFormat && frame.header.gain_code == static_cast<int>(codec.model_id() & 0xFFu);
}

float learned_picture(const LearnedCodec& codec, const LearnedUnpacked& frame, const LearnedRxOptions& options,
                      std::span<std::uint8_t> rgb) {
    const std::size_t n = frame.values.size();
    std::vector<float> v = frame.values;
    std::vector<float> weight(n, 1.0f);  // 各個值的可靠程度

    // 參考的雜訊變異數：解碼器會被告知「每個格點的雜訊都是這麼大」
    double ref = 0.0;
    if (n > 0 && options.median_snr) {
        std::vector<float> sorted = frame.noise_var;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n / 2), sorted.end());
        ref = sorted[n / 2];
    } else if (n > 0) {
        for (float s : frame.noise_var) ref += s;
        ref /= static_cast<double>(n);
    }
    // 參考值不低於解碼器認得的最高訊雜比對應的雜訊（和訓練時的 reference_noise 相同）
    ref = std::max(ref, std::pow(10.0, -static_cast<double>(kLearnedMaxSnrDb) / 10.0));
    // 解碼器認得的最低訊雜比對應的雜訊變異數。比它還吵的時候參考值停在這裡，下面的壓小會接手。
    if (options.floor) ref = std::min(ref, std::pow(10.0, -static_cast<double>(codec.min_snr_db()) / 10.0));

    if (options.attenuate) {
        // 每個實數的訊號變異數是 1/2、雜訊變異數是 σ²/2，最小均方誤差的權重是 1/(1+σ²)。
        // 雜訊等於參考值的格點解碼器自己會處理，所以只把更差的格點壓到相對於參考值的權重；
        // 比參考值好的格點不動（放大它會讓訊號的幅度不對）。
        // 壓了多少也交給解碼器（結構 2 的模型會用）：它知道哪些值不可信，就不會把被壓成接近 0 的值當真。
        for (std::size_t i = 0; i < n; ++i) {
            const double s = frame.noise_var[i];
            if (s > ref) {
                weight[i] = static_cast<float>((1.0 + ref) / (1.0 + s));
                v[i] *= weight[i];
            }
        }
    }

    const float snr_db = static_cast<float>(-10.0 * std::log10(ref));
    codec.decode(v, weight, snr_db, rgb);
    return snr_db;
}

}  // namespace nbtv
