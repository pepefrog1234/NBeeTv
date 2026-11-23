// 位元層的小工具：位元組與位元互轉、CRC、導頻的 PRBS。
// 位元一律以「一個 uint8_t 放一個位元（0 或 1）」表示，位元組內高位元在前。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nbtv {

// 位元組展開成位元（高位元在前），寫入 out；out 的長度必須是 bytes.size()*8。
void bytes_to_bits(std::span<const std::uint8_t> bytes, std::span<std::uint8_t> out);

// 位元收回成位元組；bits.size() 必須是 8 的倍數。
void bits_to_bytes(std::span<const std::uint8_t> bits, std::span<std::uint8_t> out);

// CRC-32（IEEE 802.3，與 zlib 的 crc32 相同：反射式、初值與結尾互斥或都是 0xFFFFFFFF），權重檔的檢查碼與模型的識別碼使用。
std::uint32_t crc32(std::span<const std::uint8_t> bytes);

// CRC-16/CCITT-FALSE（多項式 0x1021、初值 0xFFFF、不反射），幀的標頭使用。
std::uint16_t crc16(std::span<const std::uint8_t> bytes);

// 導頻用的 PRBS：X^11 + X^2 + 1，暫存器初值全 1，以 FFT 格號索引。
// w[k] = 1（k < 11）；w[k] = w[k-9] xor w[k-11]（k >= 11）。
// 回傳 BPSK 值：w = 0 是 +1，w = 1 是 -1。
std::array<std::int8_t, 128> pilot_prbs();

}  // namespace nbtv
