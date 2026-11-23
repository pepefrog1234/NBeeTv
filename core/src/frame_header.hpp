// 幀的標頭：每一幀開頭的 kHeaderCells 個資料格（交錯之前的順序），帶著呼號、幀序號，以及畫面是用哪一種排法、哪一份模型編的。
// 畫面本身不靠標頭也還原得出來；標頭壞了只是少了這些資訊。
#pragma once

#include <span>
#include <string>

#include "modes.hpp"
#include "types.hpp"

namespace nbtv {

// 11 位元組：版本與格式 1、幀序號 1、呼號 6、模型識別碼的最低 8 位元 1、CRC-16 2。
struct FrameHeader {
    int version = 0;       // 0..15，目前是 0
    int format = 0;        // 0..15：畫面的值在幀裡的排法，見 kLearnedFrameFormat（learned_frame.hpp）
    int sequence = 0;      // 0..255
    std::string callsign;
    bool callsign_valid = true;
    int model_tag = 0;     // 0..255：發射端所用模型的識別碼（LearnedCodec::model_id）的最低 8 位元
};

// 11 位元組 → K=7、碼率 1/2 的咬尾迴旋碼 → QPSK，整份重複兩次，共 kHeaderCells 格（平均功率 1）。
void header_encode(const FrameHeader& header, std::span<cf> cells);

// cells、noise_var 是那 kHeaderCells 格等化後的值與複數雜訊變異數。兩份的軟值相加後解碼、檢查 CRC-16；
// 成功時填入 header 並回傳 true，失敗時 header 不動。
bool header_decode(std::span<const cf> cells, std::span<const float> noise_var, FrameHeader& header);

}  // namespace nbtv
