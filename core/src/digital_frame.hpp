// 數位模式的成幀：酬載 ↔ 一幀的資料格。
// 對應規格草案「數位模式：通道編碼與交錯」的發射端流程 1–8。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "modes.hpp"
#include "types.hpp"

namespace nbtv {

inline constexpr int kProtocolVersion = 0;

// 標頭第一個位元組的低 4 位元：影像格式。
enum class ImageFormat : int {
    Av1 = 0,   // AV1 幀內編碼的 OBU 資料（序列標頭加畫面），不含時序分隔與檔案外殼
    Jpeg = 1,  // 保留給固定量化表與霍夫曼表的 baseline JPEG，尚未實作
};

// 8 位元組的標頭：版本與影像格式 1、幀序號 1、呼號 6。
struct DigitalHeader {
    int version = kProtocolVersion;  // 0..15
    int image_format = 0;            // 0..15，見 ImageFormat
    int sequence = 0;                // 0..255
    std::string callsign;            // 最多 9 個字元
    bool callsign_valid = true;      // 解碼時：呼號欄位是否為合法的數值
};

// 編成 mode.data_cells() 個格點，依傳送順序排列（已做格點交錯，尾端是補足格）。
// image 超過 mode.image_bytes() 的部分會被截掉，不足的補零。
std::vector<cf> digital_encode(const Mode& mode, const DigitalHeader& header, std::span<const std::uint8_t> image);

struct DigitalDecoded {
    bool crc_ok = false;
    bool ldpc_converged = false;
    int ldpc_iterations = 0;
    DigitalHeader header;
    std::vector<std::uint8_t> image;  // mode.image_bytes() 個位元組，含尾端的補零
};

// cells 是等化後的資料格（至少 mode.interleaver_cells() 個，依傳送順序），
// noise_var 是各格點等化後的複數雜訊變異數。
DigitalDecoded digital_decode(const Mode& mode, std::span<const cf> cells, std::span<const float> noise_var);

}  // namespace nbtv
