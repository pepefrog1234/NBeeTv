// 核心內部共用的型別與常數。
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>

namespace nbtv {

using cf = std::complex<float>;   // 訊號路徑上的複數：解析訊號、OFDM 格點
using cd = std::complex<double>;  // 設計濾波器、解線性方程式時用

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

inline constexpr int kImageWidth = 96;
inline constexpr int kImageHeight = 72;
inline constexpr int kImagePixels = kImageWidth * kImageHeight;  // 6912
inline constexpr int kImageRgbBytes = kImagePixels * 3;          // 20736

}  // namespace nbtv
