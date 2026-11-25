// 瀑布圖：把核心算出來的頻譜列（dBFS，257 點）畫成一張點陣圖，最新的在最上面。照 macOS 版的 Waterfall.swift。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "core_bridge.hpp"

namespace nbtvgui {

class WaterfallStore {
public:
    /// 這一列的亮度要以什麼為基準。
    enum class Reference {
        noise,      // 沒有同步：把目前的頻譜當成雜訊，慢慢更新雜訊基準
        hold,       // 已同步：基準固定，訊號比雜訊強多少就亮多少
        transmit,   // 發射監看：沒有雜訊可比，直接讓訊號落在偏亮的位置
    };

    static constexpr int kWidth = kSpectrumBins;
    static constexpr int kHeight = 512;        // 保留的列數
    static constexpr int kRowsPerLine = 2;     // 兩列頻譜平均成圖上的一列：約每秒 12 列
    static constexpr double kLinesPerSecond = kSampleRate / 4.0 / 512.0 / kRowsPerLine;

    WaterfallStore();
    void clear();
    /// 加入一列頻譜（dBFS）。湊滿 kRowsPerLine 列後畫出一列，回傳 true 表示圖有更新。
    bool append(const std::array<float, kSpectrumBins>& row, Reference reference);

    /// 目前的圖：寬 257、高 512，每個像素四個位元組（R、G、B、A），最新的在第 0 列。
    const std::vector<std::uint32_t>& pixels() const { return pixels_; }
    /// 圖每更新一次加一（介面用它判斷要不要重新上傳貼圖）。
    std::uint64_t version() const { return version_; }

private:
    float level(const std::vector<float>& line, Reference reference);
    void draw(const std::vector<float>& line, Reference reference);

    std::vector<std::uint32_t> pixels_;
    std::vector<float> pending_;
    int pending_count_ = 0;
    std::optional<float> noise_reference_;   // 雜訊在通帶內的中位數（dB）
    std::vector<float> recent_medians_;
    std::optional<float> transmit_reference_;
    std::uint64_t version_ = 0;
};

}  // namespace nbtvgui
