// K=7（171、133）碼率 1/2 的咬尾迴旋碼：相干類比的標頭使用。
// 咬尾：編碼器的起始狀態取訊息的最後 6 個位元，所以不需要補尾位元，n 個位元編成 2n 個。
#pragma once

#include <cstdint>
#include <span>

namespace nbtv {

// info 長度 n（至少 6），coded 長度 2n。
void conv_encode_tailbiting(std::span<const std::uint8_t> info, std::span<std::uint8_t> coded);

// 軟判決 Viterbi 解碼。llr 長度 2n，正值表示位元較可能是 0；info 長度 n。
// 作法是把序列繞三圈解碼、取中間一圈的結果，接近最大概似。
void conv_decode_tailbiting(std::span<const float> llr, std::span<std::uint8_t> info);

}  // namespace nbtv
