// 96×72 畫面的色彩轉換與整張的二維 DCT（相干類比模式用）。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "types.hpp"

namespace nbtv {

inline constexpr int kPlanes = 3;                              // Y、Cb、Cr
inline constexpr int kCoefficients = kPlanes * kImagePixels;   // 20736

// 係數的編號：plane·6912 + v·96 + u（u 是水平頻率 0..95，v 是垂直頻率 0..71）。
constexpr int coefficient_index(int plane, int u, int v) { return plane * kImagePixels + v * kImageWidth + u; }

// RGB（8 位元，96×72×3）→ Y、Cb、Cr 三個平面，都以 0 為中心（Y 已減去 128），全範圍的 BT.601。
// 輸出長度 20736：先是整個 Y 平面，接著 Cb、Cr。
void rgb_to_ycbcr(std::span<const std::uint8_t> rgb, std::span<float> planes);

// 反向轉換，結果四捨五入並限制在 0..255。
void ycbcr_to_rgb(std::span<const float> planes, std::span<std::uint8_t> rgb);

// 三個平面各自做正交歸一的二維 DCT-II（輸入、輸出長度都是 20736，可以是同一塊記憶體以外的緩衝）。
void dct_forward(std::span<const float> planes, std::span<float> coefficients);
void dct_inverse(std::span<const float> coefficients, std::span<float> planes);

}  // namespace nbtv
