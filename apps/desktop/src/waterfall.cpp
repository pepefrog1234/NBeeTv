#include "waterfall.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nbtvgui {

namespace {

constexpr float kRange = 38;             // 色階從最暗到最亮相當於幾 dB
constexpr float kNoiseLevel = 0.27f;     // 雜訊的中位數畫在色階的這個位置（暗的青色紋理）
constexpr float kSignalLevel = 0.72f;    // 發射監看時訊號畫在這個位置
constexpr float kMaxSignalLevel = 0.88f; // 接收時訊號（第 80 百分位）最亮畫到這裡
constexpr float kSilenceLevel = -95;     // 通帶的中位數低於這個 dBFS 就當作沒有輸入
constexpr std::size_t kReferenceDelay = 16;   // 雜訊基準只採用這麼多列之前的資料（約 1.4 秒）

/// 色階：深藍黑 → 藍綠 → 青 → 淡黃。記憶體裡的順序是 R、G、B、A。
const std::array<std::uint32_t, 256>& palette() {
    static const std::array<std::uint32_t, 256> table = [] {
        struct Stop {
            float at, r, g, b;
        };
        const Stop stops[] = {
            {0.00f, 5, 7, 10}, {0.15f, 8, 38, 54}, {0.35f, 14, 84, 100}, {0.55f, 36, 150, 150},
            {0.75f, 100, 214, 200}, {0.90f, 200, 236, 180}, {1.00f, 250, 244, 170},
        };
        const int count = static_cast<int>(sizeof stops / sizeof stops[0]);
        std::array<std::uint32_t, 256> colors{};
        for (int index = 0; index < 256; ++index) {
            const float t = static_cast<float>(index) / 255.0f;
            Stop a = stops[0], b = stops[count - 1];
            for (int i = 0; i + 1 < count; ++i) {
                if (t >= stops[i].at && t <= stops[i + 1].at) {
                    a = stops[i];
                    b = stops[i + 1];
                    break;
                }
            }
            const float f = b.at > a.at ? (t - a.at) / (b.at - a.at) : 0.0f;
            const std::uint8_t rgba[4] = {static_cast<std::uint8_t>(a.r + (b.r - a.r) * f), static_cast<std::uint8_t>(a.g + (b.g - a.g) * f),
                                          static_cast<std::uint8_t>(a.b + (b.b - a.b) * f), 255};
            std::memcpy(&colors[static_cast<std::size_t>(index)], rgba, 4);
        }
        return colors;
    }();
    return table;
}

}  // namespace

WaterfallStore::WaterfallStore() { clear(); }

void WaterfallStore::clear() {
    pixels_.assign(static_cast<std::size_t>(kWidth) * kHeight, palette()[0]);
    pending_.assign(static_cast<std::size_t>(kWidth), 0.0f);
    pending_count_ = 0;
    noise_reference_.reset();
    recent_medians_.clear();
    transmit_reference_.reset();
    ++version_;
}

bool WaterfallStore::append(const std::array<float, kSpectrumBins>& row, Reference reference) {
    if (pending_count_ == 0) std::fill(pending_.begin(), pending_.end(), 0.0f);
    for (std::size_t i = 0; i < row.size(); ++i) pending_[i] += std::pow(10.0f, row[i] / 10.0f);
    ++pending_count_;
    if (pending_count_ < kRowsPerLine) return false;
    std::vector<float> line(row.size());
    for (std::size_t i = 0; i < line.size(); ++i) line[i] = 10.0f * std::log10(std::max(pending_[i] / static_cast<float>(pending_count_), 1e-14f));
    pending_count_ = 0;
    draw(line, reference);
    return true;
}

/// 這一列以哪個 dB 值當作雜訊的位置。
float WaterfallStore::level(const std::vector<float>& line, Reference reference) {
    // 只看電台通帶內（約 300–2700 Hz）的部分
    const int first = static_cast<int>(300 / kSpectrumMaxHz * (kWidth - 1));
    const int last = static_cast<int>(2700 / kSpectrumMaxHz * (kWidth - 1));
    std::vector<float> sorted(line.begin() + first, line.begin() + last + 1);
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted[sorted.size() / 2];
    const float p80 = sorted[sorted.size() * 4 / 5];
    // 完全沒有輸入（取樣全是 0）：畫成全黑，也不拿來更新基準
    if (median < kSilenceLevel) return median + kNoiseLevel * kRange;
    // 訊號比基準強太多時（例如沒有雜訊的虛擬音效卡）壓住上限，免得整片飽和
    const float ceiling = p80 - (kMaxSignalLevel - kNoiseLevel) * kRange;
    switch (reference) {
    case Reference::noise: {
        transmit_reference_.reset();
        // 接收機要花一秒左右才會宣告同步；這段時間的頻譜其實已經是訊號。
        // 所以雜訊基準只採用夠舊的資料，一旦同步就把還沒採用的丟掉（見 hold）。
        recent_medians_.push_back(median);
        if (recent_medians_.size() > kReferenceDelay) {
            const float old = recent_medians_.front();
            recent_medians_.erase(recent_medians_.begin());
            noise_reference_ = noise_reference_ ? *noise_reference_ + 0.15f * (old - *noise_reference_) : old;
        }
        const float fallback = recent_medians_.empty() ? median : *std::min_element(recent_medians_.begin(), recent_medians_.end());
        return std::max(noise_reference_.value_or(fallback), ceiling);
    }
    case Reference::hold:
        transmit_reference_.reset();
        recent_medians_.clear();
        // 還沒看過雜訊就同步了（一開機就有訊號）：假設訊號比雜訊強 15 dB
        if (!noise_reference_) noise_reference_ = median - 15;
        return std::max(*noise_reference_, ceiling);
    case Reference::transmit:
        break;
    }
    recent_medians_.clear();
    const float target = p80 - (kSignalLevel - kNoiseLevel) * kRange;
    const float smoothed = transmit_reference_ ? *transmit_reference_ + 0.2f * (target - *transmit_reference_) : target;
    transmit_reference_ = smoothed;
    return smoothed;
}

void WaterfallStore::draw(const std::vector<float>& line, Reference reference) {
    const float base = level(line, reference);
    // 往下捲一列，新的一列畫在最上面
    std::memmove(pixels_.data() + kWidth, pixels_.data(), static_cast<std::size_t>(kHeight - 1) * kWidth * sizeof(std::uint32_t));
    for (int x = 0; x < kWidth; ++x) {
        const float value = kNoiseLevel + (line[static_cast<std::size_t>(x)] - base) / kRange;
        pixels_[static_cast<std::size_t>(x)] = palette()[static_cast<std::size_t>(std::min(std::max(value, 0.0f), 1.0f) * 255)];
    }
    ++version_;
}

}  // namespace nbtvgui
