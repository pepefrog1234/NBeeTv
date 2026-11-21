// 相干類比的係數順序、增益與重複配置。
//
// 規格草案把順序表與增益表列為待定（原本由 31 張影像統計而來，只是暫用）。這裡改用一個參數模型，
// 用整數算術就能在任何平台上得到完全相同的順序與配置，不需要一張兩萬項的表：
//
//   係數 (平面 p, 水平頻率 u, 垂直頻率 v) 的平均能量 λ = A / K，K = W_p · ((3u)² + (4v)² + 1)
//
// (3u)² + (4v)² 把兩個方向換成同樣的空間頻率單位（96 = 4·24、72 = 3·24）；W 是 Y 1、Cb 8、Cr 8。
// 順序：K 由小到大，相同時依係數編號。常數由 36 張影像以 tools/design/design_analog 擬合；
// 以同一批影像估算，畫質比逐係數的統計表低約 0.6 dB。
#pragma once

#include <cstdint>
#include <vector>

#include "modes.hpp"

namespace nbtv {

inline constexpr double kAnalogEnergyScale = 8.7e6;  // 模型裡的 A
inline constexpr int kAnalogChromaWeight = 8;        // W_Cb = W_Cr

struct AnalogPlan {
    int coefficients = 0;                    // 傳送的係數個數 n
    int slots = 0;                           // 可放的實數個數（每個格點兩個）
    std::vector<std::uint16_t> index;        // 第 i 個傳送的係數的編號（plane·6912 + v·96 + u）
    std::vector<std::uint16_t> copies;       // 第 i 個係數重複的份數 m_i
    std::vector<float> gain;                 // 增益 g_i = λ_i^(−1/4)
    std::vector<float> lambda;               // 平均能量 λ_i
    std::vector<std::uint16_t> slot_coef;    // 每個實數位置放的是第幾個係數
    double nominal_energy = 0.0;             // Σ g_i²·λ_i：畫面能量為平均值時，增益後的總能量
    double nominal_gain = 0.0;               // 名目幀增益：使每個實數的均方值為 1/2
};

// 某個類比模式的配置；第一次使用時建立，之後共用。
const AnalogPlan& analog_plan(const Mode& mode);

}  // namespace nbtv
