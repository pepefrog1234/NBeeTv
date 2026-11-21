// 相干類比：迴旋碼、DCT 與色彩轉換、係數配置、編解碼。
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "analog_frame.hpp"
#include "analog_tables.hpp"
#include "check.hpp"
#include "conv_code.hpp"
#include "dct.hpp"
#include "rng.hpp"
#include "test_images.hpp"

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

void test_dct_and_color() {
    Rng rng(8);
    std::vector<float> x(kCoefficients), c(kCoefficients), back(kCoefficients);
    for (auto& v : x) v = static_cast<float>(100.0 * rng.gauss());
    dct_forward(x, c);
    dct_inverse(c, back);
    double err = 0, ex = 0, ec = 0;
    for (int i = 0; i < kCoefficients; ++i) {
        err = std::max<double>(err, std::fabs(back[static_cast<std::size_t>(i)] - x[static_cast<std::size_t>(i)]));
        ex += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
        ec += static_cast<double>(c[static_cast<std::size_t>(i)]) * c[static_cast<std::size_t>(i)];
    }
    CHECK_NEAR(err, 0.0, 0.02);
    CHECK_NEAR(ec / ex, 1.0, 1e-4);  // 正交歸一：能量不變
    // 常數平面只有直流係數：值是 平均 × sqrt(像素數)
    std::fill(x.begin(), x.end(), 0.0f);
    std::fill(x.begin(), x.begin() + kImagePixels, 10.0f);
    dct_forward(x, c);
    CHECK_NEAR(c[0], 10.0 * std::sqrt(static_cast<double>(kImagePixels)), 0.01);
    CHECK_NEAR(c[1], 0.0, 1e-3);
    CHECK_NEAR(c[static_cast<std::size_t>(coefficient_index(1, 0, 0))], 0.0, 1e-3);
    // 水平方向的餘弦落在對應的 u
    for (int y = 0; y < kImageHeight; ++y) {
        for (int xx = 0; xx < kImageWidth; ++xx) x[static_cast<std::size_t>(y * kImageWidth + xx)] = static_cast<float>(std::cos((2 * xx + 1) * 5 * kPi / (2.0 * kImageWidth)));
    }
    dct_forward(x, c);
    const int peak = static_cast<int>(std::max_element(c.begin(), c.begin() + kImagePixels, [](float p, float q) { return std::fabs(p) < std::fabs(q); }) - c.begin());
    CHECK_EQ(peak, coefficient_index(0, 5, 0));

    // 色彩轉換來回誤差不超過 1
    std::vector<std::uint8_t> rgb(kImageRgbBytes), rgb2(kImageRgbBytes);
    for (auto& v : rgb) v = static_cast<std::uint8_t>(rng.below(256));
    rgb_to_ycbcr(rgb, x);
    ycbcr_to_rgb(x, rgb2);
    int worst = 0;
    for (int i = 0; i < kImageRgbBytes; ++i) worst = std::max(worst, std::abs(static_cast<int>(rgb[static_cast<std::size_t>(i)]) - rgb2[static_cast<std::size_t>(i)]));
    CHECK(worst <= 1);
    // 灰色：色度為 0，Y 以 0 為中心
    std::fill(rgb.begin(), rgb.end(), std::uint8_t{128});
    rgb_to_ycbcr(rgb, x);
    CHECK_NEAR(x[0], 0.0, 1e-3);
    CHECK_NEAR(x[kImagePixels], 0.0, 1e-3);
    CHECK_NEAR(x[2 * kImagePixels], 0.0, 1e-3);
}

void test_plan() {
    for (const Mode& m : kModes) {
        if (m.is_digital()) continue;
        const AnalogPlan& p = analog_plan(m);
        CHECK_EQ(p.slots, m.analog_values());
        CHECK(p.coefficients > 0 && p.coefficients <= p.slots);
        CHECK_EQ(static_cast<int>(p.slot_coef.size()), p.slots);
        long total = 0;
        int repeated = 0, max_copies = 0;
        for (int i = 0; i < p.coefficients; ++i) {
            total += p.copies[static_cast<std::size_t>(i)];
            repeated += p.copies[static_cast<std::size_t>(i)] > 1 ? 1 : 0;
            max_copies = std::max<int>(max_copies, p.copies[static_cast<std::size_t>(i)]);
        }
        CHECK_EQ(total, p.slots);  // 每個位置都用到
        // 每個係數在位置表裡出現的次數等於它的份數
        std::vector<int> seen(static_cast<std::size_t>(p.coefficients), 0);
        for (std::uint16_t i : p.slot_coef) ++seen[i];
        bool match = true;
        for (int i = 0; i < p.coefficients; ++i) match = match && seen[static_cast<std::size_t>(i)] == p.copies[static_cast<std::size_t>(i)];
        CHECK(match);
        // 順序：第一個是 Y 的直流；能量遞減、增益遞增；係數不重複
        CHECK_EQ(p.index[0], coefficient_index(0, 0, 0));
        bool ordered = true;
        for (int i = 1; i < p.coefficients; ++i) {
            ordered = ordered && p.lambda[static_cast<std::size_t>(i)] <= p.lambda[static_cast<std::size_t>(i - 1)] &&
                      p.gain[static_cast<std::size_t>(i)] >= p.gain[static_cast<std::size_t>(i - 1)];
        }
        CHECK(ordered);
        std::vector<std::uint16_t> sorted(p.index.begin(), p.index.end());
        std::sort(sorted.begin(), sorted.end());
        CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
        // 單一數值的名目均方根不超過平均的 2 倍：(sqrt(λ)/m) ≤ 4 × 平均功率
        const double mean_power = p.nominal_energy / p.slots;
        bool bounded = true;
        for (int i = 0; i < p.coefficients; ++i) {
            bounded = bounded && std::sqrt(static_cast<double>(p.lambda[static_cast<std::size_t>(i)])) / p.copies[static_cast<std::size_t>(i)] <= 4.0 * mean_power * 1.0001;
        }
        CHECK(bounded);
        // 重複的各份分散在不同格點：m 份至少佔 m/2 個格點（一個格點最多放兩份，I 與 Q）
        std::vector<int> cells_used(static_cast<std::size_t>(p.coefficients), 0);
        for (int t = 0; t < p.slots; ++t) {
            const std::uint16_t i = p.slot_coef[static_cast<std::size_t>(t)];
            if (t % 2 == 0 || p.slot_coef[static_cast<std::size_t>(t - 1)] != i) ++cells_used[i];
        }
        bool spread = true;
        for (int i = 0; i < p.coefficients; ++i) {
            const int m = p.copies[static_cast<std::size_t>(i)];
            spread = spread && cells_used[static_cast<std::size_t>(i)] >= (m + 1) / 2 && (m > 3 || cells_used[static_cast<std::size_t>(i)] == m);
        }
        CHECK(spread);
        int planes[3] = {0, 0, 0};
        for (int i = 0; i < p.coefficients; ++i) ++planes[p.index[static_cast<std::size_t>(i)] / kImagePixels];
        std::printf("  模式 %d：%d 個位置傳 %d 個係數（全部的 %.0f%%；Y %d、Cb %d、Cr %d），%d 個重複、最多 %d 份\n", m.id, p.slots,
                    p.coefficients, 100.0 * p.coefficients / kCoefficients, planes[0], planes[1], planes[2], repeated, max_copies);
    }
    CHECK_NEAR(analog_gain_from_code(128), 1.0, 1e-12);
    CHECK_NEAR(20 * std::log10(analog_gain_from_code(132)), 1.0, 1e-9);  // 四級是 1 dB
}

void test_codec() {
    Rng rng(21);
    for (const Mode& m : kModes) {
        if (m.is_digital()) continue;
        for (const char* name : test_images::kNames) {
            const auto rgb = test_images::load(name);
            AnalogHeader hdr;
            hdr.sequence = 77;
            hdr.callsign = "N0CALL";
            const AnalogEncoded enc = analog_encode(m, hdr, rgb);
            CHECK_EQ(static_cast<int>(enc.cells.size()), m.data_cells());
            // 格點平均功率接近 1（幀增益往下取整，最多低 0.25 dB）
            double power = 0;
            for (const cf& c : enc.cells) power += std::norm(c);
            power /= static_cast<double>(enc.cells.size());
            CHECK(power <= 1.001 && power > 0.93);

            // 沒有雜訊：解出來的畫面就是預覽（只受係數截斷影響）
            std::vector<float> var(enc.cells.size(), 1e-6f);
            const AnalogDecoded clean = analog_decode(m, enc.cells, var, -1);
            CHECK(clean.header_ok);
            CHECK_EQ(clean.header.sequence, 77);
            CHECK(clean.header.callsign == "N0CALL");
            CHECK_EQ(clean.header.gain_code, enc.gain_code);
            CHECK(test_images::psnr(clean.rgb, enc.preview) > 45.0);
            const double p_clean = test_images::psnr(clean.rgb, rgb);

            // 格點訊雜比 10 dB 與 20 dB
            double p_noisy[2];
            int q = 0;
            for (double snr_db : {10.0, 20.0}) {
                const double s2 = std::pow(10.0, -snr_db / 10.0);
                std::vector<cf> noisy(enc.cells);
                for (cf& c : noisy) c += cf(rng.cgauss() * std::sqrt(s2));
                std::vector<float> v2(noisy.size(), static_cast<float>(s2));
                const AnalogDecoded d = analog_decode(m, noisy, v2, -1);
                CHECK(d.header_ok);
                p_noisy[q++] = test_images::psnr(d.rgb, rgb);
            }
            CHECK(p_noisy[0] < p_noisy[1] && p_noisy[1] <= p_clean + 0.1);
            CHECK(p_noisy[0] > 18.0);
            std::printf("  模式 %2d %-6s：增益碼 %3d，PSNR 無雜訊 %.1f dB、20 dB 時 %.1f、10 dB 時 %.1f\n", m.id, name, enc.gain_code, p_clean,
                        p_noisy[1], p_noisy[0]);

            // 標頭那 176 格全壞：畫面仍在；用名目增益時對比有偏差，沿用上一幀的增益就正常
            std::vector<cf> broken(enc.cells);
            std::vector<float> v3(broken.size(), 1e-4f);
            for (int i = 0; i < kAnalogHeaderCells; ++i) {
                const std::size_t pos = static_cast<std::size_t>(m.interleaved_position(i));
                broken[pos] = cf(rng.cgauss());
                v3[pos] = 1.0f;
            }
            const AnalogDecoded lost = analog_decode(m, broken, v3, enc.gain_code);
            CHECK(!lost.header_ok);
            CHECK(test_images::psnr(lost.rgb, rgb) > p_clean - 0.5);
            const AnalogDecoded nominal = analog_decode(m, broken, v3, -1);
            CHECK(test_images::psnr(nominal.rgb, rgb) > 12.0);
        }
    }
    // 全黑與全灰的畫面不會出問題
    for (std::uint8_t level : {std::uint8_t{0}, std::uint8_t{128}, std::uint8_t{255}}) {
        const Mode& m = *find_mode(9);
        std::vector<std::uint8_t> flat(kImageRgbBytes, level);
        const AnalogEncoded enc = analog_encode(m, AnalogHeader{}, flat);
        bool finite = true;
        for (const cf& c : enc.cells) finite = finite && std::isfinite(c.real()) && std::isfinite(c.imag());
        CHECK(finite);
        const AnalogDecoded d = analog_decode(m, enc.cells, std::vector<float>(enc.cells.size(), 1e-4f), -1);
        CHECK(d.header_ok);
        CHECK(test_images::psnr(d.rgb, flat) > 40.0);
    }
}

}  // namespace

int main() {
    test_conv_code();
    test_dct_and_color();
    test_plan();
    test_codec();
    return check::finish("analog");
}
