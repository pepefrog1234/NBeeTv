// 共用訊號處理：48 kHz 與 12 kHz 之間的 4:1 重取樣，以及實數音訊轉成解析訊號的濾波器。
// 都是串流式的：可以一次餵任意長度，內部記住濾波器的狀態。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "types.hpp"

namespace nbtv {

// 外面來的取樣在進到任何濾波器之前先整理過：非有限值（NaN、無限大）當成 0，離譜的值夾在 ±kSampleLimit。
// 真的音效卡給不出這種值，但虛擬音訊裝置、浮點格式的錄音檔可以；它們一旦進到搜尋同步訊號用的滑動平均裡
// 就再也出不來——接收機從此收不到任何幀（要 reset 才恢復），音量、訊雜比、頻譜也跟著變成非數值。
// 上限取滿刻度的 100 倍（+40 dB）：模擬時訊號加上很強的雜訊也到不了，平方、累加之後在 float 裡也還很安全。
inline constexpr float kSampleLimit = 100.0f;

inline float clean_sample(float s) { return std::isfinite(s) ? std::clamp(s, -kSampleLimit, kSampleLimit) : 0.0f; }

// 把一段取樣整理後放進 out（取代 out 原來的內容）。
inline void clean_samples(std::span<const float> in, std::vector<float>& out) {
    out.resize(in.size());
    std::transform(in.begin(), in.end(), out.begin(), clean_sample);
}

// Kaiser 窗的 sinc 低通濾波器。cutoff 是 −6 dB 點（以取樣率為 1 的正規化頻率），直流增益為 1。
std::vector<double> design_lowpass(int taps, double cutoff, double beta);

// 48 kHz → 12 kHz。通帶到 3 kHz，9 kHz 以上（會疊進 0–3 kHz 的部分）壓 70 dB 以上。
class Decimator4 {
public:
    Decimator4();
    void reset();
    // 每 4 個輸入產生 1 個輸出，接在 out 後面。
    void process(std::span<const float> in, std::vector<float>& out);

private:
    std::vector<float> taps_;
    std::vector<float> history_;  // 環狀緩衝
    std::size_t pos_ = 0;
    int phase_ = 0;
};

// 12 kHz → 48 kHz。
class Interpolator4 {
public:
    Interpolator4();
    void reset();
    // 每個輸入產生 4 個輸出，接在 out 後面。
    void process(std::span<const float> in, std::vector<float>& out);
    // 濾波器的群延遲（以 48 kHz 的取樣數計）；發射結束時補這麼多個 0 進去就能把尾巴送完。
    int delay_samples() const { return static_cast<int>(taps_.size() - 1) / 2; }

private:
    std::vector<float> taps_;
    std::vector<float> history_;  // 12 kHz 的輸入歷史
    std::size_t pos_ = 0;
};

// 實數 → 解析訊號（只留正頻率）。通帶約 220–2780 Hz，涵蓋訊號頻帶加上 ±150 Hz 的頻偏；
// 負頻率的鏡像壓 40 dB，之後才能用複數旋轉修正頻偏而不讓鏡像漏進來。
class AnalyticFilter {
public:
    AnalyticFilter();
    void reset();
    void process(std::span<const float> in, std::vector<cf>& out);
    int delay_samples() const { return static_cast<int>(taps_.size() - 1) / 2; }

private:
    std::vector<cf> taps_;
    std::vector<float> history_;
    std::size_t pos_ = 0;
};

}  // namespace nbtv
