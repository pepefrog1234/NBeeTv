#include "conv_code.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <vector>

namespace nbtv {

namespace {

constexpr int kStates = 64;
constexpr unsigned kPoly0 = 0171;  // 八進位
constexpr unsigned kPoly1 = 0133;

constexpr unsigned parity(unsigned v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

// 暫存器 reg：bit 6 是目前輸入的位元，bit 5..0 是之前的 6 個位元（愈高愈新）。
constexpr unsigned output_bits(unsigned reg) { return (parity(reg & kPoly0) << 1) | parity(reg & kPoly1); }

}  // namespace

void conv_encode_tailbiting(std::span<const std::uint8_t> info, std::span<std::uint8_t> coded) {
    const std::size_t n = info.size();
    assert(n >= 6 && coded.size() == 2 * n);
    unsigned state = 0;
    for (std::size_t i = n - 6; i < n; ++i) state = (state >> 1) | (static_cast<unsigned>(info[i] & 1) << 5);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned reg = (static_cast<unsigned>(info[i] & 1) << 6) | state;
        const unsigned out = output_bits(reg);
        coded[2 * i] = static_cast<std::uint8_t>(out >> 1);
        coded[2 * i + 1] = static_cast<std::uint8_t>(out & 1);
        state = reg >> 1;
    }
}

void conv_decode_tailbiting(std::span<const float> llr, std::span<std::uint8_t> info) {
    const std::size_t n = info.size();
    assert(n >= 6 && llr.size() == 2 * n);
    const std::size_t steps = 3 * n;
    std::array<float, kStates> metric{}, next{};
    std::vector<std::uint8_t> decision(steps * kStates);  // 每個新狀態選了哪個前一狀態（最低位元）

    for (std::size_t t = 0; t < steps; ++t) {
        const float l0 = llr[2 * (t % n)], l1 = llr[2 * (t % n) + 1];
        // 輸出位元 (o0, o1) 的分支量度：位元是 0 加上 LLR，是 1 減去 LLR
        const float bm[4] = {l0 + l1, l0 - l1, -l0 + l1, -l0 - l1};
        for (int ns = 0; ns < kStates; ++ns) {
            const unsigned bit = static_cast<unsigned>(ns) >> 5;       // 這一步的輸入位元
            const unsigned low = (static_cast<unsigned>(ns) & 31u) << 1;  // 前一狀態去掉最舊位元的部分
            float best = -1e30f;
            std::uint8_t choice = 0;
            for (unsigned x = 0; x < 2; ++x) {
                const unsigned prev = low | x;
                const float m = metric[prev] + bm[output_bits((bit << 6) | prev)];
                if (m > best) {
                    best = m;
                    choice = static_cast<std::uint8_t>(x);
                }
            }
            next[static_cast<std::size_t>(ns)] = best;
            decision[t * kStates + static_cast<std::size_t>(ns)] = choice;
        }
        // 量度減去最大值，避免繞很多圈後數值過大
        const float top = *std::max_element(next.begin(), next.end());
        for (int s = 0; s < kStates; ++s) metric[static_cast<std::size_t>(s)] = next[static_cast<std::size_t>(s)] - top;
    }

    unsigned state = static_cast<unsigned>(std::max_element(metric.begin(), metric.end()) - metric.begin());
    for (std::size_t t = steps; t-- > 0;) {
        if (t >= n && t < 2 * n) info[t - n] = static_cast<std::uint8_t>(state >> 5);
        state = ((state & 31u) << 1) | decision[t * kStates + state];
    }
}

}  // namespace nbtv
