// 發射端：把影像酬載（數位）或畫面（類比）做成一幀 12 kHz 的音訊取樣，並管理幀序號。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "modes.hpp"

namespace nbtv {

class Transmitter {
public:
    void set_callsign(std::string callsign) { callsign_ = std::move(callsign); }
    const std::string& callsign() const { return callsign_; }

    // 包絡峰均比目標（dB）；0 表示用各模式的預設值，負值表示不削波。
    void set_papr_db(float papr_db) { papr_db_ = papr_db; }

    // 下一幀會用的幀序號（0..255，每送一幀加一）
    int sequence() const { return sequence_; }
    void set_sequence(int sequence) { sequence_ = sequence & 0xFF; }

    // 數位：image_data 是影像酬載（AV1 的 OBU 資料），最多 mode.image_bytes() 個位元組。
    std::vector<float> digital_frame(const Mode& mode, std::span<const std::uint8_t> image_data, int image_format = 0);

    // 類比：rgb 是 96×72×3 的畫面。preview 不是空指標時，填入沒有雜訊時對方看到的畫面。
    std::vector<float> analog_frame(const Mode& mode, std::span<const std::uint8_t> rgb, std::vector<std::uint8_t>* preview = nullptr);

private:
    float papr_for(const Mode& mode) const;

    std::string callsign_;
    float papr_db_ = 0.0f;
    int sequence_ = 0;
};

}  // namespace nbtv
