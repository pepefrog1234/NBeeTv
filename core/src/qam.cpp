#include "qam.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace nbtv {

namespace {

// 一個維度上的 PAM：bits_per_axis 個位元（高位在前）對到振幅。
// Gray 標記：0…0 是最大的正值，依序 +7 +5 +3 +1 −1 −3 −5 −7 對應 000 001 011 010 110 111 101 100。
struct Pam {
    int bits;         // 每個維度的位元數：1、2、3
    int levels;       // 2^bits
    float scale;      // 正規化係數
    float value[8];   // 以位元標記為索引的振幅（已乘上 scale）
};

Pam make_pam(int bits_per_cell) {
    Pam p{};
    p.bits = bits_per_cell / 2;
    p.levels = 1 << p.bits;
    const double norm = bits_per_cell == 2 ? std::sqrt(2.0) : bits_per_cell == 4 ? std::sqrt(10.0) : std::sqrt(42.0);
    p.scale = static_cast<float>(1.0 / norm);
    for (int label = 0; label < p.levels; ++label) {
        // Gray 碼轉回二進位，得到由大到小的排名
        int rank = 0;
        for (int g = label; g != 0; g >>= 1) rank ^= g;
        p.value[label] = static_cast<float>((p.levels - 1 - 2 * rank) / norm);
    }
    return p;
}

const Pam& pam_for(int bits_per_cell) {
    static const Pam qpsk = make_pam(2);
    static const Pam qam16 = make_pam(4);
    static const Pam qam64 = make_pam(6);
    assert(bits_per_cell == 2 || bits_per_cell == 4 || bits_per_cell == 6);
    return bits_per_cell == 2 ? qpsk : bits_per_cell == 4 ? qam16 : qam64;
}

// 一個維度的 log-MAP：r 是接收值，inv_var = 1/(複數雜訊變異數)。
// 每個實數維度的雜訊變異數是複數的一半，所以指數是 −(r−s)²/noise_var。
void pam_llr(const Pam& p, float r, float inv_var, float* llr, int stride) {
    float metric[8];
    for (int label = 0; label < p.levels; ++label) {
        const float d = r - p.value[label];
        metric[label] = -d * d * inv_var;
    }
    for (int b = 0; b < p.bits; ++b) {
        const int mask = 1 << (p.bits - 1 - b);
        float max0 = -1e30f, max1 = -1e30f;
        for (int label = 0; label < p.levels; ++label) {
            if (label & mask) max1 = std::max(max1, metric[label]);
            else max0 = std::max(max0, metric[label]);
        }
        float sum0 = 0.0f, sum1 = 0.0f;
        for (int label = 0; label < p.levels; ++label) {
            if (label & mask) sum1 += std::exp(metric[label] - max1);
            else sum0 += std::exp(metric[label] - max0);
        }
        llr[b * stride] = (max0 + std::log(sum0)) - (max1 + std::log(sum1));
    }
}

}  // namespace

void qam_map(std::span<const std::uint8_t> bits, int bits_per_cell, std::span<cf> cells) {
    assert(bits.size() == cells.size() * static_cast<std::size_t>(bits_per_cell));
    const Pam& p = pam_for(bits_per_cell);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const std::uint8_t* y = bits.data() + i * static_cast<std::size_t>(bits_per_cell);
        int re = 0, im = 0;
        for (int b = 0; b < p.bits; ++b) {
            re = (re << 1) | y[2 * b];
            im = (im << 1) | y[2 * b + 1];
        }
        cells[i] = cf(p.value[re], p.value[im]);
    }
}

void qam_demap(std::span<const cf> cells, std::span<const float> noise_var, int bits_per_cell, std::span<float> llr) {
    assert(noise_var.size() == cells.size());
    assert(llr.size() == cells.size() * static_cast<std::size_t>(bits_per_cell));
    const Pam& p = pam_for(bits_per_cell);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        float* out = llr.data() + i * static_cast<std::size_t>(bits_per_cell);
        const float inv_var = 1.0f / std::max(noise_var[i], 1e-9f);
        pam_llr(p, cells[i].real(), inv_var, out, 2);
        pam_llr(p, cells[i].imag(), inv_var, out + 1, 2);
    }
}

}  // namespace nbtv
