// QPSK、16QAM、64QAM 的 Gray 對應與軟解對應。星座與位元標記同 DVB-T2（EN 302 755 第 6.2.2 節）：
// 每個格點的位元 y0 y1 y2…，偶數位（y0、y2、y4）決定實部，奇數位決定虛部，平均功率為 1。
#pragma once

#include <cstdint>
#include <span>

#include "types.hpp"

namespace nbtv {

// bits 的長度是 cells.size() * bits_per_cell。
void qam_map(std::span<const std::uint8_t> bits, int bits_per_cell, std::span<cf> cells);

// 軟解對應（精確的 log-MAP）。cells 是等化後的格點，noise_var 是各格點等化後的複數雜訊變異數。
// llr 的長度是 cells.size() * bits_per_cell，正值表示位元較可能是 0。
void qam_demap(std::span<const cf> cells, std::span<const float> noise_var, int bits_per_cell, std::span<float> llr);

}  // namespace nbtv
