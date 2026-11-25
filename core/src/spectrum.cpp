#include "spectrum.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "dsp.hpp"

namespace nbtv {

Spectrum::Spectrum() : fft_(kSpectrumFft), window_(kSpectrumFft) {
    for (int n = 0; n < kSpectrumFft; ++n) {
        window_[static_cast<std::size_t>(n)] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * n / kSpectrumFft));
    }
    buffer_.reserve(2 * kSpectrumFft);
}

void Spectrum::reset() {
    buffer_.clear();
    rows_.clear();
}

void Spectrum::push(std::span<const float> samples) {
    // 非有限值當成 0（見 dsp.hpp 的 clean_sample）：不然一個 NaN 會讓包含它的那兩列整列都是 NaN
    for (float s : samples) buffer_.push_back(clean_sample(s));
    std::vector<cf> x(kSpectrumFft);
    std::size_t pos = 0;
    while (buffer_.size() - pos >= static_cast<std::size_t>(kSpectrumFft)) {
        for (int n = 0; n < kSpectrumFft; ++n) {
            x[static_cast<std::size_t>(n)] = cf(buffer_[pos + static_cast<std::size_t>(n)] * window_[static_cast<std::size_t>(n)], 0.0f);
        }
        fft_.forward(x.data());
        std::vector<float> row(kSpectrumBins);
        // 滿刻度的正弦波在 Hann 窗下的峰值是 N/4，以它當 0 dB
        const float ref = static_cast<float>(kSpectrumFft) / 4.0f;
        for (int k = 0; k < kSpectrumBins; ++k) {
            const float power = std::norm(x[static_cast<std::size_t>(k)]) / (ref * ref);
            row[static_cast<std::size_t>(k)] = 10.0f * std::log10(std::max(power, 1e-14f));
        }
        rows_.push_back(std::move(row));
        if (rows_.size() > kSpectrumMaxRows) rows_.pop_front();
        pos += kSpectrumHop;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
}

bool Spectrum::poll(std::span<float> db) {
    assert(db.size() == static_cast<std::size_t>(kSpectrumBins));
    if (rows_.empty()) return false;
    std::copy(rows_.front().begin(), rows_.front().end(), db.begin());
    rows_.pop_front();
    return true;
}

}  // namespace nbtv
