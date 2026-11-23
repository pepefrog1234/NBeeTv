// 幀裡每個格點的位置與已知值：前導符號 S0、S1，以及酬載符號裡的導頻與資料格。
// 對應規格草案的「幀結構與導頻」一節；草案標為待定的項目在這裡先定案：
//   * 導頻 PRBS 的起始狀態：全 1（同 DVB-T），以 FFT 格號索引
//   * S0：25 個偶數格上的二次相位序列（包絡峰均比 2.6 dB）
//   * S1：16 條 BPSK 序列，任兩條的內積為 ±1，編號就是模式編號
#pragma once

#include <cstdint>
#include <vector>

#include "ofdm_params.hpp"
#include "types.hpp"

namespace nbtv {

// 各種格點的振幅。資料格的平均功率是 1。
inline constexpr float kPilotAmplitude = 1.41421356f;  // 導頻比資料格高 3 dB
// 酬載符號的平均功率：三個符號一輪，共 122 個資料格與 25 個導頻，(122 + 25·2)/3 = 172/3。
// 前導符號的總功率取同一個值。
inline constexpr double kMeanSymbolPower = 172.0 / 3.0;
inline constexpr float kS1Amplitude = 1.08169683f;     // sqrt(172/147)：49 個載波
inline constexpr float kS0Amplitude = 1.51437556f;     // sqrt(172/75)：25 個載波

inline constexpr int kS0Carriers = (kCarriers + 1) / 2;  // 25：第 8、10、…、56 格
inline constexpr int kModeIdCount = 16;                  // S1 能表示的模式編號 0..15

struct CellPos {
    std::uint16_t symbol;  // 酬載符號編號，從 0 起算
    std::uint8_t bin;      // FFT 格號
};

// 導頻的值（已含振幅）。只跟格號有關。
float pilot_value(int bin);

// S0 在第 bin 格的值（已含振幅）；奇數格與頻帶外是 0。
cf s0_value(int bin);

// S1 在第 bin 格的值（已含振幅），mode_id 是 0..15；頻帶外是 0。
float s1_value(int mode_id, int bin);

// 酬載裡所有資料格的位置，依傳送順序（時間先後、頻率由低到高）排列。
std::vector<CellPos> data_cell_positions(int payload_symbols);

}  // namespace nbtv
