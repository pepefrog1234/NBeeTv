#include "analog_frame.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

#include "analog_tables.hpp"
#include "bits.hpp"
#include "callsign.hpp"
#include "conv_code.hpp"
#include "dct.hpp"
#include "qam.hpp"

namespace nbtv {

namespace {

constexpr int kHeaderBytes = 11;
constexpr int kHeaderBits = kHeaderBytes * 8;        // 88
constexpr int kHeaderCodedBits = 2 * kHeaderBits;    // 176
constexpr int kHeaderRepeats = 2;
constexpr int kGainCodeZero = 128;                   // 幀增益等於名目值的編碼
constexpr double kGainStepsPerDecade = 80.0;         // 每一級 0.25 dB：20·log10 的 1/0.25

static_assert(kHeaderCodedBits * kHeaderRepeats / 2 == kAnalogHeaderCells);

std::array<std::uint8_t, kHeaderBytes> pack_header(const AnalogHeader& h) {
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    bytes[0] = static_cast<std::uint8_t>(((h.version & 0xF) << 4) | (h.format & 0xF));
    bytes[1] = static_cast<std::uint8_t>(h.sequence & 0xFF);
    const auto call = encode_callsign(h.callsign);
    std::copy(call.begin(), call.end(), bytes.begin() + 2);
    bytes[8] = static_cast<std::uint8_t>(std::clamp(h.gain_code, 0, 255));
    const std::uint16_t crc = crc16(std::span<const std::uint8_t>(bytes.data(), 9));
    bytes[9] = static_cast<std::uint8_t>(crc >> 8);
    bytes[10] = static_cast<std::uint8_t>(crc & 0xFF);
    return bytes;
}

}  // namespace

double analog_gain_from_code(int code) { return std::pow(10.0, (code - kGainCodeZero) / kGainStepsPerDecade); }

AnalogEncoded analog_encode(const Mode& mode, const AnalogHeader& header, std::span<const std::uint8_t> rgb) {
    assert(!mode.is_digital());
    const AnalogPlan& plan = analog_plan(mode);
    const std::size_t n = static_cast<std::size_t>(plan.coefficients);

    // 1–2. 色彩轉換與整張的 DCT
    std::vector<float> planes(kCoefficients), coef(kCoefficients);
    rgb_to_ycbcr(rgb, planes);
    dct_forward(planes, coef);

    // 3–4. 取前 n 個係數、乘上增益
    std::vector<double> scaled(n);
    double energy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        scaled[i] = static_cast<double>(plan.gain[i]) * coef[plan.index[i]];
        energy += scaled[i] * scaled[i];
    }

    // 6. 幀增益：讓每個實數的均方值是 1/2（格點的平均功率與數位模式的資料格相同）。
    // 量化成 0.25 dB 一級並往下取，所以實際功率不超過名目值。
    AnalogEncoded out;
    out.coefficients = plan.coefficients;
    int code = 255;
    if (energy > 0.0) {
        const double exact = std::sqrt(0.5 * plan.slots / energy);
        code = static_cast<int>(std::floor(kGainCodeZero + kGainStepsPerDecade * std::log10(exact / plan.nominal_gain)));
        code = std::clamp(code, 0, 255);
    }
    out.gain_code = code;
    const double frame_gain = plan.nominal_gain * analog_gain_from_code(code);

    // 標頭：迴旋碼後重複兩次，QPSK
    AnalogHeader h = header;
    h.gain_code = code;
    const auto bytes = pack_header(h);
    std::array<std::uint8_t, kHeaderBits> info{};
    std::array<std::uint8_t, kHeaderCodedBits> coded{};
    bytes_to_bits(bytes, info);
    conv_encode_tailbiting(info, coded);
    std::vector<cf> plain(static_cast<std::size_t>(mode.data_cells()));
    for (int r = 0; r < kHeaderRepeats; ++r) {
        qam_map(coded, 2, std::span<cf>(plain.data() + r * (kHeaderCodedBits / 2), kHeaderCodedBits / 2));
    }

    // 5、7. 重複的各份除以 sqrt(份數)，兩個數值組成一個格點的 I 與 Q
    const std::size_t slots = static_cast<std::size_t>(plan.slots);
    for (std::size_t t = 0; t < slots; t += 2) {
        auto value = [&](std::size_t slot) {
            const std::size_t i = plan.slot_coef[slot];
            return static_cast<float>(frame_gain * scaled[i] / std::sqrt(static_cast<double>(plan.copies[i])));
        };
        plain[static_cast<std::size_t>(kAnalogHeaderCells) + t / 2] = cf(value(t), value(t + 1));
    }

    // 格點交錯
    out.cells.resize(plain.size());
    for (int i = 0; i < mode.data_cells(); ++i) out.cells[static_cast<std::size_t>(mode.interleaved_position(i))] = plain[static_cast<std::size_t>(i)];

    // 預覽：只留傳送的係數重建
    std::vector<float> kept(kCoefficients, 0.0f);
    for (std::size_t i = 0; i < n; ++i) kept[plan.index[i]] = coef[plan.index[i]];
    dct_inverse(kept, planes);
    out.preview.resize(kImageRgbBytes);
    ycbcr_to_rgb(planes, out.preview);
    return out;
}

AnalogDecoded analog_decode(const Mode& mode, std::span<const cf> cells, std::span<const float> noise_var,
                            int fallback_gain_code) {
    assert(!mode.is_digital());
    assert(static_cast<int>(cells.size()) == mode.data_cells() && noise_var.size() == cells.size());
    const AnalogPlan& plan = analog_plan(mode);
    const std::size_t n = static_cast<std::size_t>(plan.coefficients);

    // 還原格點交錯
    std::vector<cf> plain(cells.size());
    std::vector<float> var(cells.size());
    for (int i = 0; i < mode.data_cells(); ++i) {
        const std::size_t pos = static_cast<std::size_t>(mode.interleaved_position(i));
        plain[static_cast<std::size_t>(i)] = cells[pos];
        var[static_cast<std::size_t>(i)] = std::max(noise_var[pos], 1e-12f);
    }

    AnalogDecoded out;

    // 標頭：兩份的軟值相加後做 Viterbi 解碼，再檢查 CRC-16
    {
        std::array<float, kHeaderCodedBits> llr{};
        std::array<float, kHeaderCodedBits> part{};
        for (int r = 0; r < kHeaderRepeats; ++r) {
            const std::size_t first = static_cast<std::size_t>(r * (kHeaderCodedBits / 2));
            qam_demap(std::span<const cf>(plain.data() + first, kHeaderCodedBits / 2),
                      std::span<const float>(var.data() + first, kHeaderCodedBits / 2), 2, part);
            for (int b = 0; b < kHeaderCodedBits; ++b) llr[static_cast<std::size_t>(b)] += part[static_cast<std::size_t>(b)];
        }
        std::array<std::uint8_t, kHeaderBits> info{};
        conv_decode_tailbiting(llr, info);
        std::array<std::uint8_t, kHeaderBytes> bytes{};
        bits_to_bytes(info, bytes);
        const std::uint16_t crc = static_cast<std::uint16_t>((bytes[9] << 8) | bytes[10]);
        out.header_ok = crc == crc16(std::span<const std::uint8_t>(bytes.data(), 9));
        if (out.header_ok) {
            out.header.version = bytes[0] >> 4;
            out.header.format = bytes[0] & 0xF;
            out.header.sequence = bytes[1];
            std::array<std::uint8_t, kCallsignBytes> call{};
            std::copy_n(bytes.begin() + 2, kCallsignBytes, call.begin());
            const auto decoded = decode_callsign(call);
            out.header.callsign_valid = decoded.has_value();
            out.header.callsign = decoded.value_or(std::string());
            out.header.gain_code = bytes[8];
        }
    }

    const int gain_code = out.header_ok ? out.header.gain_code : (fallback_gain_code >= 0 ? fallback_gain_code : kGainCodeZero);
    const double relative_gain = analog_gain_from_code(gain_code);
    const double frame_gain = plan.nominal_gain * relative_gain;
    // 這張圖的能量相對於平均值的倍數：增益愈小表示畫面能量愈大
    const double kappa = 1.0 / (relative_gain * relative_gain);

    // 同一個係數的各份以雜訊變異數的倒數加權合併。每個實數維度的雜訊變異數是複數的一半。
    std::vector<double> num(n, 0.0), den(n, 0.0);
    const std::size_t slots = static_cast<std::size_t>(plan.slots);
    for (std::size_t t = 0; t < slots; ++t) {
        const std::size_t cell = static_cast<std::size_t>(kAnalogHeaderCells) + t / 2;
        const double value = (t % 2 == 0) ? plain[cell].real() : plain[cell].imag();
        const double weight = 2.0 / var[cell];
        const std::size_t i = plan.slot_coef[t];
        num[i] += value * weight;
        den[i] += weight;
    }

    // 線性最小均方誤差估計：雜訊大的係數自動往 0 收斂。沒傳的係數是 0。
    std::vector<float> coef(kCoefficients, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        if (den[i] <= 0.0) continue;
        const double copies = plan.copies[i];
        const double x_hat = (num[i] / den[i]) * std::sqrt(copies) / frame_gain;   // 增益後的係數 g·c
        const double x_var = copies / (den[i] * frame_gain * frame_gain);
        const double prior = kappa * std::sqrt(static_cast<double>(plan.lambda[i]));  // g²·λ = sqrt(λ)
        coef[plan.index[i]] = static_cast<float>(prior / (prior + x_var) * x_hat / plan.gain[i]);
    }

    std::vector<float> planes(kCoefficients);
    dct_inverse(coef, planes);
    out.rgb.resize(kImageRgbBytes);
    ycbcr_to_rgb(planes, out.rgb);
    return out;
}

}  // namespace nbtv
