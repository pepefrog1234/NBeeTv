// 發射端：把影像酬載（數位）或畫面（類比、學習式）做成一幀 12 kHz 的音訊取樣，並管理幀序號。
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "learned_frame.hpp"
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

    // 學習式模式用的模型。每個模式用「實數個數和它相同」的那一個，所以要用幾個學習式模式就設定幾個；
    // 個數相同的再設定一次會取代前一個。可以和接收機共用同一份；傳空指標表示全部拿掉。
    void set_model(std::shared_ptr<const LearnedCodec> model) { models_.set(std::move(model)); }
    // 這個模式會用的模型；沒有的話是空指標。
    const LearnedCodec* model_for(const Mode& mode) const { return models_.find(mode); }

    // 學習式：rgb 是 96×72×3 的畫面。preview 不是空指標時，填入沒有雜訊時對方看到的畫面（要多跑一次解碼器）。
    // 沒有這個模式用的模型時回傳空的，幀序號不變。
    std::vector<float> learned_frame(const Mode& mode, std::span<const std::uint8_t> rgb, std::vector<std::uint8_t>* preview = nullptr);

private:
    float papr_for(const Mode& mode) const;

    std::string callsign_;
    float papr_db_ = 0.0f;
    int sequence_ = 0;
    LearnedModels models_;
};

}  // namespace nbtv
