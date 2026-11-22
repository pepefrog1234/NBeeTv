// 數位模式的影像編解碼：96×72 的 RGB ↔ 影像酬載（AV1 幀內編碼、4:4:4）。
// 以 libavif（編碼用 libaom，解碼用 dav1d 或 libaom）實作；建置時沒有 libavif 的話，這些函式都回傳失敗。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace nbtv {

bool image_codec_available();

struct ImageEncodeResult {
    std::vector<std::uint8_t> payload;  // 空中傳送的 OBU 資料
    int quantizer = 0;                  // 選到的量化參數 0（最好）..63
    int encodes = 0;                    // 搜尋時實際編了幾次
};

// libaom 的速度檔 0（最慢最好）..10。畫面只有 96×72，編碼器多花一點力氣的效果很明顯：
// 同樣的位元組數，速度檔 3 比 6 高 1–3 dB，每張畫面（含位元率搜尋）的時間是 6 的五到七倍；
// 再慢下去只多不到 1 dB，時間卻再加倍以上。數字見 README 與 test_image。
// 改速度檔不影響相容性：收到的都是標準的 AV1，哪一種解碼器都能解。
inline constexpr int kDefaultEncoderSpeed = 3;

// 對量化參數做二分搜尋，取酬載不超過 max_bytes 的最佳品質（最多編 7 次）。
// rgb 是 96×72×3。量化到最粗還是放不下時回傳 false。
bool image_encode(std::span<const std::uint8_t> rgb, int max_bytes, int speed, ImageEncodeResult& result);

// 酬載（尾端可以有補零）→ 96×72×3 的 RGB。
bool image_decode(std::span<const std::uint8_t> payload, std::span<std::uint8_t> rgb);

// 以固定的量化參數編成完整的 .avif 檔（libavif 的原始輸出）。測試用：驗證酬載加回外殼後與它逐位元相同。
bool image_encode_avif_file(std::span<const std::uint8_t> rgb, int quantizer, int speed, std::vector<std::uint8_t>& file);

}  // namespace nbtv
