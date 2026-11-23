#include "bits.hpp"

#include <cassert>

namespace nbtv {

void bytes_to_bits(std::span<const std::uint8_t> bytes, std::span<std::uint8_t> out) {
    assert(out.size() == bytes.size() * 8);
    std::size_t o = 0;
    for (std::uint8_t b : bytes) {
        for (int i = 7; i >= 0; --i) out[o++] = static_cast<std::uint8_t>((b >> i) & 1);
    }
}

void bits_to_bytes(std::span<const std::uint8_t> bits, std::span<std::uint8_t> out) {
    assert(bits.size() % 8 == 0 && out.size() == bits.size() / 8);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint8_t b = 0;
        for (int j = 0; j < 8; ++j) b = static_cast<std::uint8_t>((b << 1) | (bits[i * 8 + static_cast<std::size_t>(j)] & 1));
        out[i] = b;
    }
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t b : bytes) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint16_t crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xFFFFu;
    for (std::uint8_t b : bytes) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(b) << 8));
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000u) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u) : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::array<std::int8_t, 128> pilot_prbs() {
    std::array<std::uint8_t, 128> w{};
    for (std::size_t k = 0; k < w.size(); ++k) {
        w[k] = k < 11 ? std::uint8_t{1} : static_cast<std::uint8_t>(w[k - 9] ^ w[k - 11]);
    }
    std::array<std::int8_t, 128> out{};
    for (std::size_t k = 0; k < w.size(); ++k) out[k] = w[k] ? std::int8_t{-1} : std::int8_t{1};
    return out;
}

}  // namespace nbtv
