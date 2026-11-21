// 瀑布圖用的頻譜：12 kHz 的音訊 → 每 512 個取樣一列 0–3000 Hz 的功率（dBFS）。
#pragma once

#include <deque>
#include <span>
#include <vector>

#include "fft.hpp"

namespace nbtv {

inline constexpr int kSpectrumFft = 1024;                       // 每格 11.72 Hz
inline constexpr int kSpectrumHop = 512;                        // 每秒約 23 列
inline constexpr int kSpectrumBins = kSpectrumFft / 4 + 1;      // 0–3000 Hz：257 點
inline constexpr std::size_t kSpectrumMaxRows = 64;             // 來不及取走時只留最近這麼多列

class Spectrum {
public:
    Spectrum();
    void reset();
    void push(std::span<const float> samples);  // 12 kHz
    bool poll(std::span<float> db);             // 長度 kSpectrumBins

private:
    Fft fft_;
    std::vector<float> window_;
    std::vector<float> buffer_;
    std::deque<std::vector<float>> rows_;
};

}  // namespace nbtv
