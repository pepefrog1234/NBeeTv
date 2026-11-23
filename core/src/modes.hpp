// 模式表與幀結構算術。
// OFDM 參數、前導符號與導頻沿用規格草案；模式本身都是學習式的（畫面由類神經網路變成通道上的實數，見 learned_frame.hpp）。
// 草案裡的數位模式（編號 0–5）與相干類比模式（8–10）已經移除，那些編號不再使用。
#pragma once

#include <array>
#include <numeric>

#include "ofdm_params.hpp"

namespace nbtv {

inline constexpr int kHeaderCells = 176;  // 標頭 11 位元組，迴旋碼 r=1/2 後重複兩次，QPSK（見 frame_header.hpp）

struct Mode {
    int id;                  // 空中的模式編號，由 S1 參考符號採用的序列表示
    int payload_symbols;     // 每幀的酬載符號數
    int cell_multiplier;     // 格點交錯的乘數 a：第 i 個格點放到位置 a·i mod N（N 是資料格數）
    int learned_values;      // 一張畫面變成幾個實數；模型的輸出要一樣多
    int learned_width;       // 模型第一層的通道數：實數個數相同、大小不同的模型各用一個模式

    constexpr int frame_symbols() const { return payload_symbols + kPreambleSymbols; }
    constexpr int frame_samples() const { return frame_symbols() * kSymbolSamples; }
    constexpr double frame_seconds() const {
        return static_cast<double>(frame_samples()) / kSampleRate;
    }
    constexpr int data_cells() const { return nbtv::data_cells(payload_symbols); }

    // 標頭之外的格數；每格放兩個實數
    constexpr int value_cells() const { return data_cells() - kHeaderCells; }
    constexpr int value_capacity() const { return 2 * value_cells(); }

    // 交錯前的第 i 個格點（標頭在前、畫面的值在後）在酬載資料格裡的位置。
    constexpr int interleaved_position(int i) const {
        return static_cast<int>(static_cast<long long>(cell_multiplier) * i % data_cells());
    }
};

// 每個模式各有自己的模型，實數的個數是「潛在通道數 × 9 × 12」：72、24 與 14 個通道。
// 13 的幀和 11 完全相同，只是模型的每一層加寬一倍（運算量約四倍）。
// 14 是最快的：23 個酬載符號、每張 0.667 秒；14 個通道，用的也是加寬的模型。
// 乘數取約 0.31·N、與 N 互質的數（模式 14 的 N = 935 = 5·11·17）。
inline constexpr std::array<Mode, 4> kModes{{
    // id  酬載符號  乘數  實數個數  模型寬度
    {11, 100, 1255, 7776, 32},
    {12, 37, 464, 2592, 32},
    {13, 100, 1255, 7776, 64},
    {14, 23, 291, 1512, 64},
}};

constexpr const Mode* find_mode(int id) {
    for (const Mode& m : kModes) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

// 編譯期檢查：扣掉標頭後還有空間、畫面的實數放得下、交錯乘數與格數互質。
constexpr bool mode_table_is_consistent() {
    for (const Mode& m : kModes) {
        if (std::gcd(m.cell_multiplier, m.data_cells()) != 1) return false;
        if (m.value_cells() <= 0) return false;
        if (m.learned_values <= 0 || m.learned_values % 2 != 0 || m.learned_values > m.value_capacity()) return false;
        if (m.learned_width <= 0) return false;
    }
    return true;
}
static_assert(mode_table_is_consistent());

}  // namespace nbtv
