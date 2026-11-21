#include "digital_frame.hpp"

#include <algorithm>
#include <array>
#include <cassert>

#include "bit_interleaver.hpp"
#include "bits.hpp"
#include "callsign.hpp"
#include "frame_map.hpp"
#include "ldpc.hpp"
#include "qam.hpp"

namespace nbtv {

std::vector<cf> digital_encode(const Mode& mode, const DigitalHeader& header, std::span<const std::uint8_t> image) {
    assert(mode.is_digital());
    const LdpcCode* code = LdpcCode::get(mode.rate_num, mode.rate_den);
    assert(code != nullptr && code->k() == mode.ldpc_info_bits);

    // 1. 酬載：標頭加影像資料，補零
    const std::size_t payload_bytes = static_cast<std::size_t>(mode.payload_bytes());
    std::vector<std::uint8_t> bytes(payload_bytes + 4, 0);
    bytes[0] = static_cast<std::uint8_t>(((header.version & 0xF) << 4) | (header.image_format & 0xF));
    bytes[1] = static_cast<std::uint8_t>(header.sequence & 0xFF);
    const auto call = encode_callsign(header.callsign);
    std::copy(call.begin(), call.end(), bytes.begin() + 2);
    const std::size_t image_bytes = std::min(image.size(), static_cast<std::size_t>(mode.image_bytes()));
    std::copy_n(image.begin(), image_bytes, bytes.begin() + kDigitalHeaderBytes);

    // 2. CRC-32，高位元組在前
    const std::uint32_t crc = crc32(std::span<const std::uint8_t>(bytes.data(), payload_bytes));
    for (int i = 0; i < 4; ++i) bytes[payload_bytes + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (24 - 8 * i));

    // 3. 擾碼
    std::vector<std::uint8_t> info(static_cast<std::size_t>(code->k()));
    bytes_to_bits(bytes, info);
    scramble(info);

    // 4. LDPC　5. 位元交錯　6. Gray 對應
    std::vector<std::uint8_t> codeword(kLdpcN), interleaved(kLdpcN);
    code->encode(info, codeword);
    const BitInterleaver interleaver(mode.bits_per_cell, code->k(), code->q());
    interleaver.interleave(codeword, interleaved);
    const int coded = mode.coded_cells();
    std::vector<cf> symbols(static_cast<std::size_t>(coded));
    qam_map(interleaved, mode.bits_per_cell, symbols);

    // 7. 格點交錯　8. 補足格排在最後
    std::vector<cf> cells(static_cast<std::size_t>(mode.data_cells()));
    for (int i = 0; i < coded; ++i) cells[static_cast<std::size_t>(mode.interleaved_position(i))] = symbols[static_cast<std::size_t>(i)];
    for (int p = 0; p < mode.padding_cells(); ++p) cells[static_cast<std::size_t>(coded + p)] = padding_value(p);
    return cells;
}

DigitalDecoded digital_decode(const Mode& mode, std::span<const cf> cells, std::span<const float> noise_var) {
    assert(mode.is_digital());
    const LdpcCode* code = LdpcCode::get(mode.rate_num, mode.rate_den);
    assert(code != nullptr);
    const int coded = mode.coded_cells();
    assert(static_cast<int>(cells.size()) >= coded && noise_var.size() == cells.size());

    // 還原格點交錯
    std::vector<cf> symbols(static_cast<std::size_t>(coded));
    std::vector<float> var(static_cast<std::size_t>(coded));
    for (int i = 0; i < coded; ++i) {
        const std::size_t pos = static_cast<std::size_t>(mode.interleaved_position(i));
        symbols[static_cast<std::size_t>(i)] = cells[pos];
        var[static_cast<std::size_t>(i)] = noise_var[pos];
    }

    // 軟解對應、還原位元交錯、LDPC 解碼
    std::vector<float> llr(kLdpcN), codeword_llr(kLdpcN);
    qam_demap(symbols, var, mode.bits_per_cell, llr);
    const BitInterleaver interleaver(mode.bits_per_cell, code->k(), code->q());
    interleaver.deinterleave(llr, codeword_llr);
    std::vector<std::uint8_t> bits(kLdpcN);
    const LdpcCode::Result r = code->decode(codeword_llr, bits);

    DigitalDecoded out;
    out.ldpc_converged = r.converged;
    out.ldpc_iterations = r.iterations;

    // 解擾、檢查 CRC
    std::span<std::uint8_t> info(bits.data(), static_cast<std::size_t>(code->k()));
    scramble(info);
    const std::size_t payload_bytes = static_cast<std::size_t>(mode.payload_bytes());
    std::vector<std::uint8_t> bytes(payload_bytes + 4);
    bits_to_bytes(info, bytes);
    std::uint32_t crc = 0;
    for (int i = 0; i < 4; ++i) crc = (crc << 8) | bytes[payload_bytes + static_cast<std::size_t>(i)];
    out.crc_ok = crc == crc32(std::span<const std::uint8_t>(bytes.data(), payload_bytes));

    out.header.version = bytes[0] >> 4;
    out.header.image_format = bytes[0] & 0xF;
    out.header.sequence = bytes[1];
    std::array<std::uint8_t, kCallsignBytes> call{};
    std::copy_n(bytes.begin() + 2, kCallsignBytes, call.begin());
    const auto decoded_call = decode_callsign(call);
    out.header.callsign_valid = decoded_call.has_value();
    out.header.callsign = decoded_call.value_or(std::string());
    out.image.assign(bytes.begin() + kDigitalHeaderBytes, bytes.begin() + static_cast<std::ptrdiff_t>(payload_bytes));
    return out;
}

}  // namespace nbtv
