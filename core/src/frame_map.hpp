// 幀裡每個格點的位置與已知值：前導符號 S0、S1，以及酬載符號裡的導頻與資料格。
// 對應規格草案的「幀結構與導頻」一節；草案標為待定的項目在這裡先定案：
//   * 導頻 PRBS 的起始狀態：全 1（同 DVB-T），以 FFT 格號索引
//   * S0：25 個偶數格上的二次相位序列（包絡峰均比 2.6 dB）
//   * S1：16 條 BPSK 序列，任兩條的內積為 ±1，編號就是模式編號
#pragma once

#include <cstdint>
#include <vector>

#include "modes.hpp"
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
// 沒有散佈導頻的幀（Mode::scattered_pilots 為 false）：酬載的 49 個載波都是資料格。為了讓每個符號的功率和其他符號一樣，
// 資料格的振幅取和 S1 相同（比有導頻的幀高 0.68 dB：導頻省下來的功率分給了資料格）。
inline constexpr float kFreeDataAmplitude = kS1Amplitude;
// 這種幀結尾的參考符號用的序列：S1 的第 0 條。它不是任何模式的編號（模式表裡沒有 0），所以接收機不會把它當成
// 某一幀的開頭——用和開頭相同的那一條的話，前導符號被衰落蓋掉時，結尾的這一個會被認成下一幀的 S1。
// 沒有散佈導頻的模式都用同一條；16 條序列的峰均比都不超過 6.7 dB，任兩條的內積是 ±1。
inline constexpr int kTailReferenceId = 0;

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

// 酬載裡所有資料格的位置，依傳送順序（時間先後、頻率由低到高）排列。這是有散佈導頻的幀：導頻的位置跳過。
std::vector<CellPos> data_cell_positions(int payload_symbols);
// 同上，照模式的幀種類：沒有散佈導頻的幀每個載波都是資料格。
std::vector<CellPos> data_cell_positions(const Mode& mode);

}  // namespace nbtv
