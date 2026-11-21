// 設計工具：搜尋 S1 參考符號的排列與遮罩，並評估 S0 的峰均比。
// 結果已經抄進 core/src/preamble.cpp；這支程式留著，日後要重新設計時可以重跑。
//
// S1 的 16 條序列：X_m[c] = mask[c] · (−1)^popcount(m & cls[c])，c = 0..48。
// cls 把 49 個載波分成 16 類（15 類各 3 個、1 類 4 個），所以任兩條序列的內積恰為 ±1。
// 這裡搜尋的是類別在頻率上的排列與 ±1 遮罩，目標是壓低 16 條序列的峰均比，
// 並讓錯開偶數格（整數頻偏假設錯誤）時的相關值不要太高。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "fft.hpp"
#include "ofdm_params.hpp"
#include "rng.hpp"

using namespace nbtv;

namespace {

constexpr int kOversample = 4;
const Fft g_fft(kFftSize * kOversample);

// 解析訊號包絡的峰均比（dB）。values[i] 放在第 first + i·step 格。
double papr_db(const std::vector<cf>& values, int first, int step) {
    std::vector<cf> x(static_cast<std::size_t>(g_fft.size()), cf(0, 0));
    for (std::size_t i = 0; i < values.size(); ++i) x[static_cast<std::size_t>(first) + i * static_cast<std::size_t>(step)] = values[i];
    g_fft.inverse(x.data());
    double peak = 0, mean = 0;
    for (const cf& v : x) {
        const double p = std::norm(v);
        peak = std::max(peak, p);
        mean += p;
    }
    mean /= static_cast<double>(x.size());
    return 10.0 * std::log10(peak / mean);
}

int popcount4(int v) { return ((v >> 0) & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1); }

struct Design {
    int cls[kCarriers];
    int mask[kCarriers];
};

int value(const Design& d, int mode, int c) { return (popcount4(mode & d.cls[c]) & 1) ? -d.mask[c] : d.mask[c]; }

struct Score {
    double max_papr = 0, mean_papr = 0, max_shift_corr = 0;
};

Score evaluate(const Design& d) {
    Score s;
    for (int m = 0; m < 16; ++m) {
        std::vector<cf> v(kCarriers);
        for (int c = 0; c < kCarriers; ++c) v[static_cast<std::size_t>(c)] = cf(static_cast<float>(value(d, m, c)), 0);
        const double p = papr_db(v, kFirstBin, 1);
        s.max_papr = std::max(s.max_papr, p);
        s.mean_papr += p / 16.0;
    }
    for (int a = 0; a < 16; ++a) {
        for (int b = 0; b < 16; ++b) {
            for (int shift = 2; shift <= 8; shift += 2) {
                int acc = 0;
                for (int c = 0; c + shift < kCarriers; ++c) acc += value(d, a, c + shift) * value(d, b, c);
                s.max_shift_corr = std::max(s.max_shift_corr, std::abs(acc) / static_cast<double>(kCarriers - shift));
            }
        }
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const int trials = argc > 1 ? std::atoi(argv[1]) : 20000;

    // S0：25 個偶數格上的二次相位（Zadoff–Chu 型）序列，比較幾個根值的峰均比
    std::printf("S0（偶數格 %d..%d，共 25 個）二次相位序列的峰均比：\n", kFirstBin, kLastBin);
    for (int u : {1, 2, 3, 4, 6, 7, 8, 9, 11, 12}) {
        std::vector<cf> v(25);
        for (int i = 0; i < 25; ++i) v[static_cast<std::size_t>(i)] = std::polar(1.0f, static_cast<float>(-kPi * u * i * (i + 1) / 25.0));
        std::printf("  u = %2d：%.2f dB\n", u, papr_db(v, kFirstBin, 2));
    }

    Rng rng(20261121);
    Design best{};
    double best_cost = 1e9;
    Score best_score;
    for (int t = 0; t < trials; ++t) {
        Design d{};
        // 15 類各 3 個、第 0 類 4 個，隨機排列
        std::vector<int> classes;
        for (int c = 0; c < 16; ++c) {
            for (int r = 0; r < (c == 0 ? 4 : 3); ++r) classes.push_back(c);
        }
        for (int i = kCarriers - 1; i > 0; --i) std::swap(classes[static_cast<std::size_t>(i)], classes[rng.below(static_cast<std::uint32_t>(i + 1))]);
        for (int c = 0; c < kCarriers; ++c) {
            d.cls[c] = classes[static_cast<std::size_t>(c)];
            d.mask[c] = rng.bit() ? 1 : -1;
        }
        const Score s = evaluate(d);
        const double cost = s.max_papr + 6.0 * std::max(0.0, s.max_shift_corr - 0.42);
        if (cost < best_cost) {
            best_cost = cost;
            best = d;
            best_score = s;
        }
    }
    std::printf("\nS1：%d 次嘗試中最好的一組\n", trials);
    std::printf("  16 條序列的峰均比：最大 %.2f dB、平均 %.2f dB；錯開偶數格時的最大相關 %.3f\n", best_score.max_papr,
                best_score.mean_papr, best_score.max_shift_corr);
    std::printf("  cls  = {");
    for (int c = 0; c < kCarriers; ++c) std::printf("%d%s", best.cls[c], c + 1 < kCarriers ? ", " : "};\n");
    std::printf("  mask = {");
    for (int c = 0; c < kCarriers; ++c) std::printf("%d%s", best.mask[c], c + 1 < kCarriers ? ", " : "};\n");
    // 驗算：同一格上任兩條序列的內積
    int max_dot = 0;
    for (int a = 0; a < 16; ++a) {
        for (int b = a + 1; b < 16; ++b) {
            int acc = 0;
            for (int c = 0; c < kCarriers; ++c) acc += value(best, a, c) * value(best, b, c);
            max_dot = std::max(max_dot, std::abs(acc));
        }
    }
    std::printf("  任兩條序列內積的最大絕對值：%d（49 個載波）\n", max_dot);
    return 0;
}
