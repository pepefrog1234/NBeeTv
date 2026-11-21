// 位元交錯、QAM、幀結構、數位成幀，以及 OFDM 調變（以位置已知的理想解調驗證）。
#include <algorithm>
#include <cmath>
#include <vector>

#include "bit_interleaver.hpp"
#include "check.hpp"
#include "digital_frame.hpp"
#include "fft.hpp"
#include "frame_map.hpp"
#include "ldpc.hpp"
#include "ofdm_tx.hpp"
#include "qam.hpp"
#include "rng.hpp"

using namespace nbtv;

namespace {

void test_bit_interleaver() {
    for (const Mode& m : kModes) {
        if (!m.is_digital()) continue;
        const LdpcCode* code = LdpcCode::get(m.rate_num, m.rate_den);
        const BitInterleaver il(m.bits_per_cell, code->k(), code->q());
        // 是一個排列
        std::vector<int> seen(kLdpcN, 0);
        for (std::uint16_t p : il.permutation()) ++seen[p];
        CHECK(std::all_of(seen.begin(), seen.end(), [](int c) { return c == 1; }));
        // QPSK 不交錯
        if (m.bits_per_cell == 2) {
            for (int i = 0; i < kLdpcN; ++i) CHECK_EQ(il.permutation()[static_cast<std::size_t>(i)], i);
        }
        // 交錯再還原
        Rng rng(5);
        std::vector<std::uint8_t> cw(kLdpcN), tx(kLdpcN);
        for (auto& b : cw) b = static_cast<std::uint8_t>(rng.bit());
        il.interleave(cw, tx);
        std::vector<float> llr(kLdpcN), back(kLdpcN);
        for (int i = 0; i < kLdpcN; ++i) llr[static_cast<std::size_t>(i)] = tx[static_cast<std::size_t>(i)] ? -1.0f : 1.0f;
        il.deinterleave(llr, back);
        bool same = true;
        for (int i = 0; i < kLdpcN; ++i) same = same && ((back[static_cast<std::size_t>(i)] < 0) == (cw[static_cast<std::size_t>(i)] != 0));
        CHECK(same);
    }
    // 16QAM 1/2 的幾個定點：K = 7200、q = 25、8 行 × 2025 列，tc = {0,0,0,1,7,20,20,21}，解多工 {7,1,4,2,5,3,6,0}。
    const BitInterleaver il(4, 7200, 25);
    // 第 0 列：第 c 行讀到的是該行第 (0 − tc) mod 2025 個位元；輸入第 c 個放到輸出第 demux[c] 個位置。
    CHECK_EQ(il.permutation()[7], 0);                  // 第 0 行、tc = 0 → u[0] = λ[0]，放到位置 7
    CHECK_EQ(il.permutation()[1], 2025);               // 第 1 行 → u[2025]，位置 1
    CHECK_EQ(il.permutation()[4], 2 * 2025);           // 第 2 行 → u[4050]，位置 4（還在資訊位元的範圍內）
    // 第 3 行起落在校驗位元，要再經過 parity 交錯：u[K + 360t + s] = λ[K + q·s + t]。
    // 第 3 行、tc = 1：第 0 列讀到 u[3·2025 + 2024] = u[8099]，放到位置 2；8099 − 7200 = 899 = 360·2 + 179。
    CHECK_EQ(il.permutation()[2], 7200 + 25 * 179 + 2);
    {
        const int idx = 4 * 2025 + (2025 - 7);  // 第 4 行、tc = 7，放到位置 5
        const int t = (idx - 7200) / 360, s = (idx - 7200) % 360;
        CHECK_EQ(il.permutation()[5], 7200 + 25 * s + t);
    }
}

void test_qam() {
    Rng rng(7);
    for (int bpc : {2, 4, 6}) {
        const int n = 6000;
        std::vector<std::uint8_t> bits(static_cast<std::size_t>(n * bpc));
        for (auto& b : bits) b = static_cast<std::uint8_t>(rng.bit());
        std::vector<cf> cells(static_cast<std::size_t>(n));
        qam_map(bits, bpc, cells);
        double power = 0;
        for (const cf& c : cells) power += std::norm(c);
        CHECK_NEAR(power / n, 1.0, 0.03);
        // 無雜訊時硬判決與原位元相同
        std::vector<float> var(static_cast<std::size_t>(n), 0.01f), llr(bits.size());
        qam_demap(cells, var, bpc, llr);
        bool same = true;
        for (std::size_t i = 0; i < bits.size(); ++i) same = same && ((llr[i] < 0) == (bits[i] != 0));
        CHECK(same);
        // 雜訊變異數愈大，LLR 愈小
        std::vector<float> var2(static_cast<std::size_t>(n), 1.0f), llr2(bits.size());
        qam_demap(cells, var2, bpc, llr2);
        CHECK(std::fabs(llr2[0]) < std::fabs(llr[0]));
    }
    // 位元標記：DVB-T2 的 Gray 對應。QPSK 00 在第一象限。
    {
        const std::uint8_t b00[] = {0, 0}, b11[] = {1, 1};
        cf c[1];
        qam_map(b00, 2, c);
        CHECK(c[0].real() > 0 && c[0].imag() > 0);
        qam_map(b11, 2, c);
        CHECK(c[0].real() < 0 && c[0].imag() < 0);
        // 16QAM：y0 y2 = 00 → +3，01 → +1，11 → −1，10 → −3（除以 √10）
        const float s = 1.0f / std::sqrt(10.0f);
        const std::uint8_t q[4][4] = {{0, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 1, 0}, {1, 0, 0, 0}};
        const float expect[4] = {3, 1, -1, -3};
        for (int i = 0; i < 4; ++i) {
            qam_map(q[i], 4, c);
            CHECK_NEAR(c[0].real(), expect[i] * s, 1e-6);
            CHECK_NEAR(c[0].imag(), 3 * s, 1e-6);
        }
        // 64QAM：y0 y2 y4 = 000 → +7，100 → −7，110 → −1（除以 √42）
        const float s64 = 1.0f / std::sqrt(42.0f);
        const std::uint8_t a[6] = {0, 0, 0, 0, 0, 0}, b[6] = {1, 0, 0, 0, 0, 0}, d[6] = {1, 1, 1, 1, 0, 0};
        qam_map(a, 6, c);
        CHECK_NEAR(c[0].real(), 7 * s64, 1e-6);
        qam_map(b, 6, c);
        CHECK_NEAR(c[0].real(), -7 * s64, 1e-6);
        qam_map(d, 6, c);
        CHECK_NEAR(c[0].real(), -1 * s64, 1e-6);
        CHECK_NEAR(c[0].imag(), -1 * s64, 1e-6);
    }
}

void test_frame_map() {
    // 資料格的順序與數量
    for (const Mode& m : kModes) {
        const auto cells = data_cell_positions(m.payload_symbols);
        CHECK_EQ(static_cast<int>(cells.size()), m.data_cells());
        bool ordered = true, no_pilot = true;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            no_pilot = no_pilot && !is_pilot(cells[i].symbol, cells[i].bin);
            if (i > 0) {
                ordered = ordered && (cells[i].symbol > cells[i - 1].symbol ||
                                      (cells[i].symbol == cells[i - 1].symbol && cells[i].bin > cells[i - 1].bin));
            }
        }
        CHECK(ordered);
        CHECK(no_pilot);
        // 格點交錯是排列
        std::vector<int> seen(static_cast<std::size_t>(m.interleaver_cells()), 0);
        for (int i = 0; i < m.interleaver_cells(); ++i) ++seen[static_cast<std::size_t>(m.interleaved_position(i))];
        CHECK(std::all_of(seen.begin(), seen.end(), [](int c) { return c == 1; }));
    }
    // 規格表：相鄰編碼格的間隔約 61、31、20 個符號
    CHECK_NEAR(find_mode(1)->interleaved_position(1) / 40.67, 61.0, 1.0);
    CHECK_NEAR(find_mode(3)->interleaved_position(1) / 40.67, 31.0, 0.5);
    CHECK_NEAR(find_mode(5)->interleaved_position(1) / 40.67, 20.0, 0.6);

    // 導頻：振幅 √2（比資料格高 3 dB），只與格號有關
    for (int k = kFirstBin; k <= kLastBin; ++k) CHECK_NEAR(std::fabs(pilot_value(k)), std::sqrt(2.0), 1e-6);

    // S0 只在偶數格；S0、S1 的總功率等於酬載符號的平均功率 173/3
    double p0 = 0, p1 = 0;
    for (int k = 0; k < kFftSize / 2; ++k) {
        p0 += std::norm(s0_value(k));
        p1 += s1_value(3, k) * s1_value(3, k);
        if (k < kFirstBin || k > kLastBin || k % 2 != 0) CHECK_NEAR(std::abs(s0_value(k)), 0.0, 1e-9);
        if (k < kFirstBin || k > kLastBin) CHECK_NEAR(s1_value(3, k), 0.0, 1e-9);
    }
    CHECK_NEAR(p0, kMeanSymbolPower, 1e-3);
    CHECK_NEAR(p1, kMeanSymbolPower, 1e-3);
    const double payload_power = (2 * (41 + 8 * 2) + (40 + 9 * 2)) / 3.0;
    CHECK_NEAR(payload_power, kMeanSymbolPower, 1e-9);

    // S1：16 條序列，任兩條的內積是 ±1（幾乎正交）
    for (int a = 0; a < kModeIdCount; ++a) {
        for (int b = a + 1; b < kModeIdCount; ++b) {
            double dot = 0;
            for (int k = kFirstBin; k <= kLastBin; ++k) dot += s1_value(a, k) * s1_value(b, k);
            CHECK_NEAR(std::fabs(dot) / (kS1Amplitude * kS1Amplitude), 1.0, 1e-4);
        }
    }
}

std::vector<std::uint8_t> random_bytes(Rng& rng, int n) {
    std::vector<std::uint8_t> v(static_cast<std::size_t>(n));
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.below(256));
    return v;
}

void test_digital_frame() {
    Rng rng(42);
    for (const Mode& m : kModes) {
        if (!m.is_digital()) continue;
        DigitalHeader hdr;
        hdr.sequence = 200 + m.id;
        hdr.callsign = "BX4ACP";
        const auto image = random_bytes(rng, m.image_bytes());
        const auto cells = digital_encode(m, hdr, image);
        CHECK_EQ(static_cast<int>(cells.size()), m.data_cells());
        double power = 0;
        for (const cf& c : cells) power += std::norm(c);
        CHECK_NEAR(power / static_cast<double>(cells.size()), 1.0, 0.03);

        std::vector<float> var(cells.size(), 0.05f);
        const DigitalDecoded d = digital_decode(m, cells, var);
        CHECK(d.crc_ok);
        CHECK(d.ldpc_converged);
        CHECK_EQ(d.header.version, kProtocolVersion);
        CHECK_EQ(d.header.image_format, 0);
        CHECK_EQ(d.header.sequence, 200 + m.id);
        CHECK(d.header.callsign == "BX4ACP");
        CHECK(d.image == image);

        // 加上雜訊：比各調變的門檻高一截，仍應解得出來
        const double snr_db = m.id == 0 ? 0.0 : m.id == 1 ? 4.0 : m.id == 2 ? 8.0 : m.id == 3 ? 9.0 : m.id == 4 ? 14.0 : 18.0;
        const double sigma2 = std::pow(10.0, -snr_db / 10.0);
        std::vector<cf> noisy(cells);
        for (cf& c : noisy) c += cf(rng.cgauss() * std::sqrt(sigma2));
        std::vector<float> var2(cells.size(), static_cast<float>(sigma2));
        const DigitalDecoded d2 = digital_decode(m, noisy, var2);
        CHECK(d2.crc_ok);
        CHECK(d2.image == image);
        std::printf("  模式 %d：格點訊雜比 %.0f dB，LDPC %d 次疊代\n", m.id, snr_db, d2.ldpc_iterations);

        // 全是雜訊時 CRC 不會過
        for (cf& c : noisy) c = cf(rng.cgauss());
        const DigitalDecoded d3 = digital_decode(m, noisy, std::vector<float>(cells.size(), 1.0f));
        CHECK(!d3.crc_ok);

        // 較短的影像資料會補零
        const auto short_image = random_bytes(rng, 100);
        const DigitalDecoded d4 = digital_decode(m, digital_encode(m, hdr, short_image), var);
        CHECK(d4.crc_ok);
        CHECK(std::equal(short_image.begin(), short_image.end(), d4.image.begin()));
        CHECK(std::all_of(d4.image.begin() + 100, d4.image.end(), [](std::uint8_t b) { return b == 0; }));
    }
}

// 位置已知的理想解調：直接在每個符號的有效部分做 FFT，回傳各資料格（已除以發射端的刻度）。
std::vector<cf> ideal_demod(const Mode& m, const std::vector<float>& samples, double* scale_out) {
    const Fft fft(kFftSize);
    const auto positions = data_cell_positions(m.payload_symbols);
    std::vector<cf> cells;
    cells.reserve(positions.size());
    // 先由導頻估出整體刻度（實數增益）
    double num = 0, den = 0;
    std::vector<std::vector<cf>> spectra;
    for (int j = 0; j < m.payload_symbols; ++j) {
        std::vector<cf> x(kFftSize);
        const std::size_t start = static_cast<std::size_t>((j + kPreambleSymbols) * kSymbolSamples + kCyclicPrefix);
        for (int n = 0; n < kFftSize; ++n) x[static_cast<std::size_t>(n)] = cf(samples[start + static_cast<std::size_t>(n)], 0);
        fft.forward(x.data());
        for (int k = kFirstBin; k <= kLastBin; ++k) {
            if (is_pilot(j, k)) {
                num += (x[static_cast<std::size_t>(k)] * pilot_value(k)).real();
                den += pilot_value(k) * pilot_value(k);
            }
        }
        spectra.push_back(std::move(x));
    }
    const double scale = num / den;
    *scale_out = scale;
    for (const CellPos& p : positions) cells.push_back(spectra[p.symbol][p.bin] / static_cast<float>(scale));
    return cells;
}

void test_ofdm_modulator() {
    Rng rng(99);
    for (const Mode& m : kModes) {
        if (!m.is_digital()) continue;
        DigitalHeader hdr;
        hdr.callsign = "N0CALL";
        const auto image = random_bytes(rng, m.image_bytes());
        const auto cells = digital_encode(m, hdr, image);

        // 不削波：解調回來與原格點幾乎相同
        const auto clean = ofdm_modulate(m, cells, 0.0f);
        CHECK_EQ(static_cast<int>(clean.size()), m.frame_samples());
        double scale = 0;
        const auto rx_clean = ideal_demod(m, clean, &scale);
        double err = 0;
        for (std::size_t i = 0; i < cells.size(); ++i) err += std::norm(rx_clean[i] - cells[i]);
        CHECK(10 * std::log10(err / static_cast<double>(cells.size())) < -60.0);
        // 循環字首：每個符號開頭 64 點等於結尾 64 點
        double cp_err = 0;
        for (int s = 0; s < m.frame_symbols(); ++s) {
            for (int n = 0; n < kCyclicPrefix; ++n) {
                const std::size_t a = static_cast<std::size_t>(s * kSymbolSamples + n);
                cp_err = std::max<double>(cp_err, std::fabs(clean[a] - clean[a + kFftSize]));
            }
        }
        CHECK_NEAR(cp_err, 0.0, 1e-6);
        // S0 的前後兩半相同
        double half_err = 0;
        for (int n = 0; n < kFftSize / 2; ++n) {
            half_err = std::max<double>(half_err, std::fabs(clean[static_cast<std::size_t>(kCyclicPrefix + n)] -
                                                            clean[static_cast<std::size_t>(kCyclicPrefix + n + kFftSize / 2)]));
        }
        CHECK_NEAR(half_err, 0.0, 1e-6);

        // 預設削波：量調變誤差、輸出峰值與平均功率，並確認仍解得出來
        const float papr = default_papr_db(m);
        const auto tx = ofdm_modulate(m, cells, papr);
        const auto rx = ideal_demod(m, tx, &scale);
        double e2 = 0, peak = 0, power = 0;
        for (std::size_t i = 0; i < cells.size(); ++i) e2 += std::norm(rx[i] - cells[i]);
        for (float v : tx) {
            peak = std::max<double>(peak, std::fabs(v));
            power += static_cast<double>(v) * v;
        }
        const double mer_db = -10 * std::log10(e2 / static_cast<double>(cells.size()));
        const double rms_db = 10 * std::log10(power / static_cast<double>(tx.size()));
        CHECK(peak <= 1.0);
        CHECK(mer_db > (m.bits_per_cell == 2 ? 18.0 : m.bits_per_cell == 4 ? 22.0 : 26.0));
        std::vector<float> var(rx.size(), static_cast<float>(std::pow(10.0, -mer_db / 10.0)));
        const DigitalDecoded d = digital_decode(m, rx, var);
        CHECK(d.crc_ok);
        CHECK(d.image == image);
        std::printf("  模式 %d：未削波峰均比 %.1f dB；削到 %.0f dB 後調變誤差比 %.1f dB、峰值 %.2f、均方根 %.1f dBFS\n", m.id,
                    measure_envelope_papr_db(m, cells), papr, mer_db, peak, rms_db);
    }
}

}  // namespace

int main() {
    test_bit_interleaver();
    test_qam();
    test_frame_map();
    test_digital_frame();
    test_ofdm_modulator();
    return check::finish("frame");
}
