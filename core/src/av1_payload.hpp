// 數位模式的影像酬載：空中只傳 AV1 的 OBU 資料（序列標頭加畫面），不傳 AVIF 檔的外殼與時序分隔。
// 這裡處理酬載與標準 .avif 檔之間的轉換，不需要任何外部函式庫。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nbtv {

// AVIF 檔裡固定不傳的部分：檔案外殼（ftyp、meta 與 mdat 的箱頭）275 位元組，加上 2 位元組的時序分隔 OBU。
inline constexpr std::size_t kAvifShellBytes = 275;
inline constexpr std::size_t kAvifTemporalDelimiterBytes = 2;

// 酬載裡 OBU 資料的實際長度（酬載尾端可能補了 0）。必須以序列標頭 OBU 開頭；格式不對時回傳 0。
std::size_t av1_payload_length(std::span<const std::uint8_t> payload);

// 序列標頭裡與外殼有關的欄位。
struct Av1SequenceInfo {
    int profile = 0;
    int level = 0;
    int width = 0, height = 0;
    bool high_bitdepth = false;
};
bool av1_parse_sequence_header(std::span<const std::uint8_t> payload, Av1SequenceInfo& info);

// 酬載 → 標準的 .avif 檔：補回時序分隔、套上外殼並填入長度。酬載尾端的補零會先去掉。
// 酬載不是 96×72 的 AV1 靜態畫面時回傳空的向量。
std::vector<std::uint8_t> avif_from_payload(std::span<const std::uint8_t> payload);

// .avif 檔（libavif 的輸出）→ 酬載：取出 mdat 裡的 OBU 資料並去掉時序分隔。失敗時回傳空的向量。
std::vector<std::uint8_t> payload_from_avif(std::span<const std::uint8_t> file);

}  // namespace nbtv
