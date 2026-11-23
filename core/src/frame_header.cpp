#include "frame_header.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

#include "bits.hpp"
#include "callsign.hpp"
#include "conv_code.hpp"

namespace nbtv {

namespace {

constexpr int kHeaderBytes = 11;
constexpr int kHeaderBits = kHeaderBytes * 8;        // 88
constexpr int kHeaderCodedBits = 2 * kHeaderBits;    // 176
constexpr int kHeaderRepeats = 2;
constexpr int kCopyCells = kHeaderCodedBits / 2;     // 一份佔 88 格：每格兩個位元

static_assert(kCopyCells * kHeaderRepeats == kHeaderCells);

// QPSK：每格兩個位元，第一個決定實部、第二個決定虛部；0 是 +1/√2、1 是 −1/√2，平均功率 1
// （DVB-T2 的 Gray 對應，EN 302 755 第 6.2.2 節）。
constexpr float kQpskAmplitude = 0.70710678f;

std::array<std::uint8_t, kHeaderBytes> pack_header(const FrameHeader& h) {
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    bytes[0] = static_cast<std::uint8_t>(((h.version & 0xF) << 4) | (h.format & 0xF));
    bytes[1] = static_cast<std::uint8_t>(h.sequence & 0xFF);
    const auto call = encode_callsign(h.callsign);
    std::copy(call.begin(), call.end(), bytes.begin() + 2);
    bytes[8] = static_cast<std::uint8_t>(std::clamp(h.model_tag, 0, 255));
    const std::uint16_t crc = crc16(std::span<const std::uint8_t>(bytes.data(), 9));
    bytes[9] = static_cast<std::uint8_t>(crc >> 8);
    bytes[10] = static_cast<std::uint8_t>(crc & 0xFF);
    return bytes;
}

// 一個維度的軟值（正值表示位元較可能是 0）：兩個可能的值各自的 −距離²／雜訊變異數 相減。
// 每個實數維度的雜訊變異數是複數的一半，所以分母是複數的雜訊變異數，不必再乘 2。
float axis_llr(float r, float inv_var) {
    const float d0 = r - kQpskAmplitude, d1 = r + kQpskAmplitude;
    return (-d0 * d0 * inv_var) - (-d1 * d1 * inv_var);
}

}  // namespace

void header_encode(const FrameHeader& header, std::span<cf> cells) {
    assert(static_cast<int>(cells.size()) == kHeaderCells);
    const auto bytes = pack_header(header);
    std::array<std::uint8_t, kHeaderBits> info{};
    std::array<std::uint8_t, kHeaderCodedBits> coded{};
    bytes_to_bits(bytes, info);
    conv_encode_tailbiting(info, coded);
    for (int r = 0; r < kHeaderRepeats; ++r) {
        for (int i = 0; i < kCopyCells; ++i) {
            const std::size_t bit = static_cast<std::size_t>(2 * i);
            cells[static_cast<std::size_t>(r * kCopyCells + i)] =
                cf(coded[bit] ? -kQpskAmplitude : kQpskAmplitude, coded[bit + 1] ? -kQpskAmplitude : kQpskAmplitude);
        }
    }
}

bool header_decode(std::span<const cf> cells, std::span<const float> noise_var, FrameHeader& header) {
    assert(static_cast<int>(cells.size()) == kHeaderCells && noise_var.size() == cells.size());
    std::array<float, kHeaderCodedBits> llr{};
    for (int r = 0; r < kHeaderRepeats; ++r) {
        for (int i = 0; i < kCopyCells; ++i) {
            const std::size_t cell = static_cast<std::size_t>(r * kCopyCells + i);
            const float inv_var = 1.0f / std::max(noise_var[cell], 1e-9f);
            llr[static_cast<std::size_t>(2 * i)] += axis_llr(cells[cell].real(), inv_var);
            llr[static_cast<std::size_t>(2 * i + 1)] += axis_llr(cells[cell].imag(), inv_var);
        }
    }
    std::array<std::uint8_t, kHeaderBits> info{};
    conv_decode_tailbiting(llr, info);
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    bits_to_bytes(info, bytes);
    const std::uint16_t crc = static_cast<std::uint16_t>((bytes[9] << 8) | bytes[10]);
    if (crc != crc16(std::span<const std::uint8_t>(bytes.data(), 9))) return false;
    header.version = bytes[0] >> 4;
    header.format = bytes[0] & 0xF;
    header.sequence = bytes[1];
    std::array<std::uint8_t, kCallsignBytes> call{};
    std::copy_n(bytes.begin() + 2, kCallsignBytes, call.begin());
    const auto decoded = decode_callsign(call);
    header.callsign_valid = decoded.has_value();
    header.callsign = decoded.value_or(std::string());
    header.model_tag = bytes[8];
    return true;
}

}  // namespace nbtv
