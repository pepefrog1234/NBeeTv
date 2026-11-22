// 相干類比模式：畫面 ↔ 一幀的資料格。
// 對應規格草案「類比模式：相干類比」的發射端流程 1–7 與接收端流程 2–5。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "modes.hpp"
#include "types.hpp"

namespace nbtv {

// 11 位元組的標頭：版本與格式 1、幀序號 1、呼號 6、幀增益 1、CRC-16 2。
struct AnalogHeader {
    int version = 0;
    int format = 0;        // 保留，目前是 0
    int sequence = 0;      // 0..255
    std::string callsign;
    bool callsign_valid = true;
    int gain_code = 0;     // 幀增益的量化值，見 analog_gain_from_code()
};

// 標頭的編解碼（相干類比與學習式共用）：11 位元組 → 迴旋碼 → QPSK，重複兩次，共 kAnalogHeaderCells 格。
void analog_header_encode(const AnalogHeader& header, std::span<cf> cells);
// cells、noise_var 是那 kAnalogHeaderCells 格等化後的值與複數雜訊變異數。兩份的軟值相加後解碼、檢查 CRC-16；
// 成功時填入 header 並回傳 true，失敗時 header 不動。
bool analog_header_decode(std::span<const cf> cells, std::span<const float> noise_var, AnalogHeader& header);

// 幀增益碼 → 相對於名目增益的倍數：每一級 0.25 dB，128 是 0 dB。
double analog_gain_from_code(int code);

struct AnalogEncoded {
    std::vector<cf> cells;             // mode.data_cells() 個格點，依傳送順序
    int gain_code = 0;                 // 寫進標頭的幀增益
    int coefficients = 0;              // 這個模式傳送的係數個數
    std::vector<std::uint8_t> preview; // 只用傳送的係數重建的畫面（沒有雜訊時對方看到的樣子），96×72×3
};

// rgb 是 96×72×3 的畫面。header 的 gain_code 由編碼器填入。
AnalogEncoded analog_encode(const Mode& mode, const AnalogHeader& header, std::span<const std::uint8_t> rgb);

struct AnalogDecoded {
    bool header_ok = false;
    AnalogHeader header;
    std::vector<std::uint8_t> rgb;  // 96×72×3
};

// cells 是等化後的資料格（mode.data_cells() 個，依傳送順序），noise_var 是各格點等化後的複數雜訊變異數。
// 標頭解不出來時用 fallback_gain_code 當幀增益；−1 表示用名目增益（假設畫面的能量是平均值）。
AnalogDecoded analog_decode(const Mode& mode, std::span<const cf> cells, std::span<const float> noise_var,
                            int fallback_gain_code);

}  // namespace nbtv
