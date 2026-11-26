#include "ofdm_tx.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

#include "fft.hpp"
#include "frame_map.hpp"

namespace nbtv {

namespace {

const Fft& fft256() {
    static const Fft fft(kFftSize);
    return fft;
}

// 一個符號在頻域的內容：第 kFirstBin..kLastBin 格。
using SymbolCells = std::array<cf, kCarriers>;

// 依序產生一幀的每個符號（S0、S1、酬載）在頻域的格點。
template <typename Fn>
void for_each_symbol(const Mode& mode, std::span<const cf> data_cells, Fn&& fn) {
    assert(static_cast<int>(data_cells.size()) == mode.data_cells());
    SymbolCells cells{};
    for (int k = kFirstBin; k <= kLastBin; ++k) cells[static_cast<std::size_t>(k - kFirstBin)] = s0_value(k);
    fn(cells, /*even_only=*/true);
    for (int k = kFirstBin; k <= kLastBin; ++k) cells[static_cast<std::size_t>(k - kFirstBin)] = cf(s1_value(mode.id, k), 0);
    fn(cells, false);
    std::size_t next = 0;
    for (int j = 0; j < mode.payload_symbols; ++j) {
        for (int k = kFirstBin; k <= kLastBin; ++k) {
            cf& cell = cells[static_cast<std::size_t>(k - kFirstBin)];
            if (!mode.scattered_pilots) cell = data_cells[next++] * kFreeDataAmplitude;  // 每個載波都是資料
            else cell = is_pilot(j, k) ? cf(pilot_value(k), 0) : data_cells[next++];
        }
        fn(cells, false);
    }
    assert(next == data_cells.size());
    // 沒有散佈導頻的幀：結尾再送一個已知的符號（結尾的參考符號），接收端拿它和開頭的 S1 比，估出這一幀之內的時脈漂移
    for (int t = 0; t < mode.tail_symbols(); ++t) {
        for (int k = kFirstBin; k <= kLastBin; ++k) cells[static_cast<std::size_t>(k - kFirstBin)] = cf(s1_value(kTailReferenceId, k), 0);
        fn(cells, false);
    }
}

// 頻域格點 → 解析訊號的一個有效符號（256 點）。平均包絡功率正規化成 1。
void to_time(const SymbolCells& cells, cf* z) {
    static const float norm = static_cast<float>(1.0 / std::sqrt(kMeanSymbolPower));
    std::fill(z, z + kFftSize, cf(0, 0));
    for (int c = 0; c < kCarriers; ++c) z[kFirstBin + c] = cells[static_cast<std::size_t>(c)] * norm;
    fft256().inverse(z);
}

// 包絡削波後濾掉頻帶外（以及 S0 的奇數格）的產物。
void clip_and_filter(cf* z, float limit, bool even_only) {
    bool clipped = false;
    for (int n = 0; n < kFftSize; ++n) {
        const float mag = std::abs(z[n]);
        if (mag > limit) {
            z[n] *= limit / mag;
            clipped = true;
        }
    }
    if (!clipped) return;
    fft256().forward(z);
    const float scale = 1.0f / kFftSize;
    for (int k = 0; k < kFftSize; ++k) {
        const bool keep = k >= kFirstBin && k <= kLastBin && (!even_only || (k - kFirstBin) % 2 == 0);
        z[k] = keep ? z[k] * scale : cf(0, 0);
    }
    fft256().inverse(z);
}

}  // namespace

std::vector<float> ofdm_modulate(const Mode& mode, std::span<const cf> data_cells, float papr_db) {
    const bool clip = papr_db > 0.0f;
    const float limit = std::pow(10.0f, (clip ? papr_db : 12.0f) / 20.0f);
    const float out_scale = 1.0f / (limit * kTxHeadroom);

    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(mode.frame_samples()));
    cf z[kFftSize];
    for_each_symbol(mode, data_cells, [&](const SymbolCells& cells, bool even_only) {
        to_time(cells, z);
        if (clip) {
            clip_and_filter(z, limit, even_only);
            clip_and_filter(z, limit, even_only);
        }
        auto emit = [&](int n) { out.push_back(std::clamp(z[n].real() * out_scale, -1.0f, 1.0f)); };
        for (int n = kFftSize - kCyclicPrefix; n < kFftSize; ++n) emit(n);  // 循環字首
        for (int n = 0; n < kFftSize; ++n) emit(n);
    });
    assert(static_cast<int>(out.size()) == mode.frame_samples());
    return out;
}

float measure_envelope_papr_db(const Mode& mode, std::span<const cf> data_cells) {
    double peak = 0.0, sum = 0.0;
    long count = 0;
    cf z[kFftSize];
    for_each_symbol(mode, data_cells, [&](const SymbolCells& cells, bool) {
        to_time(cells, z);
        for (int n = 0; n < kFftSize; ++n) {
            const double p = std::norm(z[n]);
            peak = std::max(peak, p);
            sum += p;
            ++count;
        }
    });
    return static_cast<float>(10.0 * std::log10(peak / (sum / static_cast<double>(count))));
}

}  // namespace nbtv
