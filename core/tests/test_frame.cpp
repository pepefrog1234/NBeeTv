// 迴旋碼、幀的標頭、幀結構，以及 OFDM 調變（以位置已知的理想解調驗證）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bits.hpp"
#include "check.hpp"
#include "conv_code.hpp"
#include "fft.hpp"
#include "frame_header.hpp"
#include "frame_map.hpp"
#include "ofdm_tx.hpp"
#include "rng.hpp"

using namespace nbtv;

namespace {

void test_conv_code() {
    Rng rng(3);
    for (int trial = 0; trial < 20; ++trial) {
        std::vector<std::uint8_t> info(88), coded(176), back(88);
        for (auto& b : info) b = static_cast<std::uint8_t>(rng.bit());
        conv_encode_tailbiting(info, coded);
        // 無雜訊
        std::vector<float> llr(176);
        for (int i = 0; i < 176; ++i) llr[static_cast<std::size_t>(i)] = coded[static_cast<std::size_t>(i)] ? -4.0f : 4.0f;
        conv_decode_tailbiting(llr, back);
        CHECK(back == info);
        // BPSK 過 AWGN，Eb/N0 = 4 dB（碼率 1/2 → Es/N0 = 1 dB）：K=7 的碼在這裡的錯誤率約 1e-4
        const double sigma = std::sqrt(1.0 / (2.0 * std::pow(10.0, 0.1)));
        for (int i = 0; i < 176; ++i) {
            const double y = (coded[static_cast<std::size_t>(i)] ? -1.0 : 1.0) + sigma * rng.gauss();
            llr[static_cast<std::size_t>(i)] = static_cast<float>(2.0 * y / (sigma * sigma));
        }
        conv_decode_tailbiting(llr, back);
        CHECK(back == info);
        // 抹掉連續 12 個軟值仍能還原
        for (int i = 0; i < 176; ++i) llr[static_cast<std::size_t>(i)] = coded[static_cast<std::size_t>(i)] ? -4.0f : 4.0f;
        for (int i = 60; i < 72; ++i) llr[static_cast<std::size_t>(i)] = 0.0f;
        conv_decode_tailbiting(llr, back);
        CHECK(back == info);
    }
    // 咬尾：訊息循環移位，碼字也跟著循環移位
    std::vector<std::uint8_t> info(88), a(176), b(176), shifted(88);
    for (auto& v : info) v = static_cast<std::uint8_t>(rng.bit());
    conv_encode_tailbiting(info, a);
    for (int i = 0; i < 88; ++i) shifted[static_cast<std::size_t>(i)] = info[static_cast<std::size_t>((i + 5) % 88)];
    conv_encode_tailbiting(shifted, b);
    bool same = true;
    for (int i = 0; i < 176; ++i) same = same && b[static_cast<std::size_t>(i)] == a[static_cast<std::size_t>((i + 10) % 176)];
    CHECK(same);
}

// 標頭那 176 格的硬判決（每格兩個位元：實部、虛部，負的是 1）收成位元組
std::vector<std::uint8_t> header_bits(const std::vector<cf>& cells) {
    std::vector<std::uint8_t> bits;
    for (const cf& c : cells) {
        bits.push_back(c.real() < 0 ? 1 : 0);
        bits.push_back(c.imag() < 0 ? 1 : 0);
    }
    std::vector<std::uint8_t> bytes(bits.size() / 8);
    bits_to_bytes(bits, bytes);
    return bytes;
}

void test_header() {
    FrameHeader h;
    h.format = 1;
    h.sequence = 200;
    h.callsign = "N0CALL";
    h.model_tag = 0xD0;
    std::vector<cf> cells(kHeaderCells);
    header_encode(h, cells);
    // QPSK：每格都在四個點上，平均功率 1；整份重複兩次
    const double a = std::sqrt(0.5);
    for (const cf& c : cells) {
        CHECK_NEAR(std::fabs(c.real()), a, 1e-6);
        CHECK_NEAR(std::fabs(c.imag()), a, 1e-6);
    }
    for (int i = 0; i < kHeaderCells / 2; ++i) CHECK(cells[static_cast<std::size_t>(i)] == cells[static_cast<std::size_t>(i + kHeaderCells / 2)]);

    // 空中的格式不能在不知不覺中改掉：這一幀的標頭編出來的位元，檢查碼是移除數位與類比模式之前算好的
    // （那時標頭和類比模式共用，程式在 analog_frame.cpp）。改了標頭的排法或編碼，兩端就不能互通。
    CHECK_EQ(crc32(header_bits(cells)), 0x182BF411u);

    // 沒有雜訊時原樣取回
    const std::vector<float> quiet(cells.size(), 0.01f);
    FrameHeader back;
    CHECK(header_decode(cells, quiet, back));
    CHECK_EQ(back.version, 0);
    CHECK_EQ(back.format, 1);
    CHECK_EQ(back.sequence, 200);
    CHECK(back.callsign == "N0CALL" && back.callsign_valid);
    CHECK_EQ(back.model_tag, 0xD0);

    // 各欄位的範圍：版本與格式各 4 個位元，幀序號與模型識別碼各 8 個位元，呼號最多 9 個字元
    for (int v : {0, 5, 15}) {
        FrameHeader x;
        x.version = v;
        x.format = 15 - v;
        x.sequence = (v * 17 + 3) & 0xFF;
        x.model_tag = 255 - v;
        x.callsign = v == 0 ? "" : v == 5 ? "VK2/N0CAL" : "W1AW/QRP";
        header_encode(x, cells);
        FrameHeader y;
        CHECK(header_decode(cells, quiet, y));
        CHECK(y.version == x.version && y.format == x.format && y.sequence == x.sequence && y.model_tag == x.model_tag);
        CHECK(y.callsign == x.callsign);
    }

    // 白雜訊：格點訊雜比 0 dB 以上幾乎都解得出來，−2 dB 時約四分之三，−12 dB 時 CRC 幾乎都不過
    header_encode(h, cells);
    Rng rng(17);
    auto noisy_ok = [&](double snr_db) {
        const double sigma2 = std::pow(10.0, -snr_db / 10.0);
        std::vector<cf> noisy(cells);
        for (cf& c : noisy) c += cf(rng.cgauss() * std::sqrt(sigma2));
        const std::vector<float> var(noisy.size(), static_cast<float>(sigma2));
        FrameHeader out;
        if (!header_decode(noisy, var, out)) return false;
        CHECK(out.sequence == 200 && out.callsign == "N0CALL" && out.model_tag == 0xD0 && out.format == 1);  // 解出來的就要是對的
        return true;
    };
    int good = 0, marginal = 0, lucky = 0;
    for (int trial = 0; trial < 200; ++trial) good += noisy_ok(0.0) ? 1 : 0;
    for (int trial = 0; trial < 200; ++trial) marginal += noisy_ok(-2.0) ? 1 : 0;
    for (int trial = 0; trial < 200; ++trial) lucky += noisy_ok(-12.0) ? 1 : 0;
    std::printf("  標頭：格點訊雜比 0 dB 時 200 次解出 %d 次，−2 dB 時 %d 次，−12 dB 時 %d 次\n", good, marginal, lucky);
    CHECK(good >= 190);                       // 量到的：2000 次裡 1989 次
    CHECK(marginal >= 120 && marginal < 180); // 量到的：2000 次裡 1481 次
    CHECK(lucky <= 2);

    // 全是雜訊：CRC 擋下來，header 不動
    int passed = 0;
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<cf> noise(cells.size());
        for (cf& c : noise) c = cf(rng.cgauss());
        FrameHeader out;
        out.sequence = 77;
        const bool ok = header_decode(noise, std::vector<float>(noise.size(), 1.0f), out);
        passed += ok ? 1 : 0;
        if (!ok) CHECK_EQ(out.sequence, 77);
    }
    CHECK(passed <= 1);  // CRC-16：每次有 1/65536 的機會剛好通過

    // 其中一份（後半的 88 格）整個壞掉、雜訊變異數也照實標出來：靠另一份照樣解得出來
    std::vector<cf> half(cells);
    std::vector<float> var(cells.size(), 0.01f);
    for (std::size_t i = cells.size() / 2; i < cells.size(); ++i) {
        half[i] = cf(rng.cgauss() * 3.0);
        var[i] = 9.0f;
    }
    CHECK(header_decode(half, var, back));
    CHECK_EQ(back.sequence, 200);
}

void test_frame_map() {
    // 資料格的順序與數量
    for (const Mode& m : kModes) {
        const auto cells = data_cell_positions(m);
        CHECK_EQ(static_cast<int>(cells.size()), m.data_cells());
        bool ordered = true, no_pilot = true;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            // 有散佈導頻的幀：資料格不落在導頻的位置上。沒有的幀每個載波都是資料格（下面另外檢查個數）。
            no_pilot = no_pilot && (!m.scattered_pilots || !is_pilot(cells[i].symbol, cells[i].bin));
            if (i > 0) {
                ordered = ordered && (cells[i].symbol > cells[i - 1].symbol ||
                                      (cells[i].symbol == cells[i - 1].symbol && cells[i].bin > cells[i - 1].bin));
            }
        }
        CHECK(ordered);
        CHECK(no_pilot);
        if (!m.scattered_pilots) CHECK_EQ(static_cast<int>(cells.size()), m.payload_symbols * kCarriers);
        // 格點交錯是排列
        std::vector<int> seen(static_cast<std::size_t>(m.data_cells()), 0);
        for (int i = 0; i < m.data_cells(); ++i) ++seen[static_cast<std::size_t>(m.interleaved_position(i))];
        CHECK(std::all_of(seen.begin(), seen.end(), [](int c) { return c == 1; }));
        // 相鄰的兩個格點（交錯之前）在幀裡隔了三成：衰落或突波抹掉的一片格點，不會落在連續的值上
        CHECK_NEAR(m.interleaved_position(1) / static_cast<double>(m.data_cells()), 0.31, 0.005);
        // 標頭的 176 格散在整幀：每個「五分之一幀」裡都有三十格上下
        int fifth[5] = {0, 0, 0, 0, 0};
        for (int i = 0; i < kHeaderCells; ++i) ++fifth[cells[static_cast<std::size_t>(m.interleaved_position(i))].symbol * 5 / m.payload_symbols];
        for (int n : fifth) CHECK(n >= 22 && n <= 50);
    }

    // 導頻：振幅 √2（比資料格高 3 dB），只與格號有關
    for (int k = kFirstBin; k <= kLastBin; ++k) CHECK_NEAR(std::fabs(pilot_value(k)), std::sqrt(2.0), 1e-6);

    // S0 只在偶數格；S0、S1 的總功率等於酬載符號的平均功率 172/3
    double p0 = 0, p1 = 0;
    for (int k = 0; k < kFftSize / 2; ++k) {
        p0 += std::norm(s0_value(k));
        p1 += s1_value(11, k) * s1_value(11, k);
        if (k < kFirstBin || k > kLastBin || k % 2 != 0) CHECK_NEAR(std::abs(s0_value(k)), 0.0, 1e-9);
        if (k < kFirstBin || k > kLastBin) CHECK_NEAR(s1_value(11, k), 0.0, 1e-9);
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
    // 模式編號都在 S1 能表示的範圍內
    for (const Mode& m : kModes) CHECK(m.id >= 0 && m.id < kModeIdCount);
}

// 位置已知的理想解調：直接在每個符號的有效部分做 FFT，回傳各資料格（已除以發射端的刻度）。
// spectra_out 不是空指標時，填入整幀每個符號的頻譜（同樣除過刻度）。
std::vector<cf> ideal_demod(const Mode& m, const std::vector<float>& samples, double* scale_out,
                            std::vector<std::vector<cf>>* spectra_out = nullptr) {
    const Fft fft(kFftSize);
    const auto positions = data_cell_positions(m);
    std::vector<cf> cells;
    cells.reserve(positions.size());
    std::vector<std::vector<cf>> spectra;
    for (int row = 0; row < m.frame_symbols(); ++row) {
        std::vector<cf> x(kFftSize);
        const std::size_t start = static_cast<std::size_t>(row * kSymbolSamples + kCyclicPrefix);
        for (int n = 0; n < kFftSize; ++n) x[static_cast<std::size_t>(n)] = cf(samples[start + static_cast<std::size_t>(n)], 0);
        fft.forward(x.data());
        spectra.push_back(std::move(x));
    }
    // 先估出整體刻度（實數增益）：有散佈導頻的幀用導頻，沒有的用開頭的 S1
    double num = 0, den = 0;
    if (m.scattered_pilots) {
        for (int j = 0; j < m.payload_symbols; ++j) {
            for (int k = kFirstBin; k <= kLastBin; ++k) {
                if (!is_pilot(j, k)) continue;
                num += (spectra[static_cast<std::size_t>(j + kPreambleSymbols)][static_cast<std::size_t>(k)] * pilot_value(k)).real();
                den += pilot_value(k) * pilot_value(k);
            }
        }
    } else {
        for (int k = kFirstBin; k <= kLastBin; ++k) {
            num += (spectra[1][static_cast<std::size_t>(k)] * s1_value(m.id, k)).real();
            den += s1_value(m.id, k) * s1_value(m.id, k);
        }
    }
    const double scale = num / den;
    *scale_out = scale;
    // 沒有散佈導頻的幀，資料格的振幅是 kFreeDataAmplitude
    const float cell_scale = static_cast<float>(scale) * (m.scattered_pilots ? 1.0f : kFreeDataAmplitude);
    for (const CellPos& p : positions) cells.push_back(spectra[static_cast<std::size_t>(p.symbol + kPreambleSymbols)][p.bin] / cell_scale);
    if (spectra_out != nullptr) {
        for (auto& row : spectra)
            for (cf& v : row) v /= static_cast<float>(scale);
        *spectra_out = std::move(spectra);
    }
    return cells;
}

// 一幀的資料格：標頭加上畫面的值。這裡不跑模型，畫面的值用高斯亂數代替（模型的輸出每個實數均方值 1/2，
// 兩個一組的格點平均功率是 1），沒用到的格點是 0，排法和 learned_encode 相同。
std::vector<cf> synthetic_cells(const Mode& m, Rng& rng) {
    std::vector<cf> plain(static_cast<std::size_t>(m.data_cells()));
    FrameHeader h;
    h.format = 1;
    h.sequence = m.id;
    h.callsign = "N0CALL";
    header_encode(h, std::span<cf>(plain.data(), static_cast<std::size_t>(kHeaderCells)));
    for (int k = 0; k < m.learned_values / 2; ++k) plain[static_cast<std::size_t>(kHeaderCells + k)] = cf(rng.cgauss());
    std::vector<cf> cells(plain.size());
    for (int i = 0; i < m.data_cells(); ++i) cells[static_cast<std::size_t>(m.interleaved_position(i))] = plain[static_cast<std::size_t>(i)];
    return cells;
}

void test_ofdm_modulator() {
    Rng rng(99);
    for (const Mode& m : kModes) {
        const auto cells = synthetic_cells(m, rng);

        // 不削波：解調回來與原格點幾乎相同
        const auto clean = ofdm_modulate(m, cells, 0.0f);
        CHECK_EQ(static_cast<int>(clean.size()), m.frame_samples());
        double scale = 0;
        std::vector<std::vector<cf>> spectra;
        const auto rx_clean = ideal_demod(m, clean, &scale, &spectra);
        double err = 0;
        for (std::size_t i = 0; i < cells.size(); ++i) err += std::norm(rx_clean[i] - cells[i]);
        CHECK(10 * std::log10(err / static_cast<double>(cells.size())) < -60.0);
        // 已知的符號：S1 是這個模式的序列；沒有散佈導頻的幀結尾多一個參考符號，用的是 S1 的第 0 條序列（不是任何模式的）
        CHECK_EQ(static_cast<int>(spectra.size()), kPreambleSymbols + m.payload_symbols + (m.scattered_pilots ? 0 : 1));
        double known_err = 0;
        for (int k = kFirstBin; k <= kLastBin; ++k) {
            known_err = std::max<double>(known_err, std::abs(spectra[1][static_cast<std::size_t>(k)] - cf(s1_value(m.id, k), 0)));
            if (!m.scattered_pilots) {
                known_err = std::max<double>(known_err, std::abs(spectra.back()[static_cast<std::size_t>(k)] - cf(s1_value(kTailReferenceId, k), 0)));
            }
        }
        CHECK_NEAR(known_err, 0.0, 1e-3);
        if (!m.scattered_pilots) {
            // 每個酬載符號的 49 個載波都帶著資料，總功率和 S1 相同（資料格平均功率 1 × kFreeDataAmplitude²）
            double payload_power = 0;
            for (int j = 0; j < m.payload_symbols; ++j) {
                for (int k = kFirstBin; k <= kLastBin; ++k) payload_power += std::norm(spectra[static_cast<std::size_t>(j + kPreambleSymbols)][static_cast<std::size_t>(k)]);
            }
            double used = 0;  // 用到的格點（標頭與畫面的值）平均功率是 1，沒用到的是 0
            for (const cf& v : cells) used += std::norm(v);
            CHECK_NEAR(payload_power / used, kFreeDataAmplitude * kFreeDataAmplitude, 1e-3);
            CHECK_NEAR(kCarriers * kFreeDataAmplitude * kFreeDataAmplitude, kMeanSymbolPower, 1e-3);
        }
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

        // 預設削波：量調變誤差、輸出峰值與平均功率，並確認標頭仍解得出來
        const auto tx = ofdm_modulate(m, cells, kDefaultPaprDb);
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
        CHECK(mer_db > 24.0);  // 削波的失真比畫面的值低 24 dB 以上
        std::vector<cf> header(static_cast<std::size_t>(kHeaderCells));
        for (int i = 0; i < kHeaderCells; ++i) header[static_cast<std::size_t>(i)] = rx[static_cast<std::size_t>(m.interleaved_position(i))];
        FrameHeader back;
        CHECK(header_decode(header, std::vector<float>(header.size(), static_cast<float>(std::pow(10.0, -mer_db / 10.0))), back));
        CHECK(back.sequence == m.id && back.callsign == "N0CALL");
        std::printf("  模式 %d：未削波峰均比 %.1f dB；削到 %.0f dB 後調變誤差比 %.1f dB、峰值 %.2f、均方根 %.1f dBFS\n", m.id,
                    measure_envelope_papr_db(m, cells), static_cast<double>(kDefaultPaprDb), mer_db, peak, rms_db);
    }
}

}  // namespace

int main() {
    test_conv_code();
    test_header();
    test_frame_map();
    test_ofdm_modulator();
    return check::finish("frame");
}
