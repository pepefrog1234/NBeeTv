// OFDM 調變：一幀的資料格 → 12 kHz 的實數音訊取樣。
// 訊號以解析訊號（只有正頻率）的形式產生，在包絡上削波並濾掉頻帶外的產物，最後取實部；
// SSB 發射機的峰包功率由這個包絡決定，所以削波做在包絡上，而不是做在音訊波形上。
#pragma once

#include <span>
#include <vector>

#include "modes.hpp"
#include "types.hpp"

namespace nbtv {

// 輸出的刻度：包絡的削波門檻對應到 1/kTxHeadroom，留一點空間給濾波後回升的峰值；
// 實數取樣的絕對值不會超過 1。
inline constexpr float kTxHeadroom = 1.2f;

// 各模式預設的包絡峰均比目標（dB）。削波門檻 = 平均包絡功率 + 這個值。
// 草案把削波方式列為待定；這裡依各調變容得下的失真取值，test_ofdm 會量測削波後的調變誤差。
constexpr float default_papr_db(const Mode& mode) {
    if (!mode.is_digital()) return 7.0f;   // 類比沒有糾錯碼吸收失真，削得輕
    return mode.bits_per_cell == 2 ? 5.0f : mode.bits_per_cell == 4 ? 6.0f : 7.0f;
}

// 把一幀調變成 mode.frame_samples() 個取樣：S0、S1、酬載符號，各含循環字首。
// data_cells 是 mode.data_cells() 個資料格，依傳送順序排列。
// papr_db <= 0 表示不削波（測試用）；此時刻度仍以 12 dB 的門檻計。
std::vector<float> ofdm_modulate(const Mode& mode, std::span<const cf> data_cells, float papr_db);

// 未削波時包絡峰值對平均功率的比（dB），量測用。
float measure_envelope_papr_db(const Mode& mode, std::span<const cf> data_cells);

}  // namespace nbtv
