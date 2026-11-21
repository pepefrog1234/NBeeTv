// 呼號編碼：最多 9 個字元、每個字元 40 種符號，合成一個 48 位元的數（6 位元組）。
// 40^9 < 2^48，所以裝得下。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nbtv {

inline constexpr int kCallsignMaxChars = 9;
inline constexpr int kCallsignBytes = 6;

// 符號表：索引 0 是空白（補在尾端），接著是數字、大寫字母，以及呼號常見的 '/'、'-'、'.'。
inline constexpr std::string_view kCallsignAlphabet = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/-.";

// 把使用者輸入整理成可以編碼的呼號：轉大寫、丟掉不在符號表裡的字元、去掉頭尾空白、截到 9 個字元。
std::string normalize_callsign(std::string_view text);

// 編碼成 6 位元組（高位元組在前）。輸入會先經過 normalize_callsign。
std::array<std::uint8_t, kCallsignBytes> encode_callsign(std::string_view text);

// 解碼；數值超出 40^9（不可能由編碼器產生）時回傳 nullopt。尾端的空白會去掉。
std::optional<std::string> decode_callsign(const std::array<std::uint8_t, kCallsignBytes>& bytes);

}  // namespace nbtv
