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

// 畫面大小由模式決定（modes.hpp 的 Mode::image_width、image_height）。
// 模式 3 與 11–14 的畫面是 96×72，下面這四個數就是這個「標準大小」；模式 4–7 與 15（FM 用的大畫面）是 192×144。
inline constexpr int kImageWidth = 96;
inline constexpr int kImageHeight = 72;
inline constexpr int kImagePixels = kImageWidth * kImageHeight;  // 6912
inline constexpr int kImageRgbBytes = kImagePixels * 3;          // 20736
// 核心處理的最大畫面：任何模式、任何權重檔的畫面都不超過這個大小
inline constexpr int kMaxImageWidth = 192;
inline constexpr int kMaxImageHeight = 144;
inline constexpr int kMaxImageRgbBytes = kMaxImageWidth * kMaxImageHeight * 3;  // 82944

}  // namespace nbtv
