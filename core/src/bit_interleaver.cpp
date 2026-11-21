#include "bit_interleaver.hpp"

#include <cassert>
#include <numeric>

#include "ldpc_tables.hpp"

namespace nbtv {

namespace {

// EN 302 755 表 7(b)：Nldpc = 16200 的 column-twist 參數 tc
constexpr int kTwist16[8] = {0, 0, 0, 1, 7, 20, 20, 21};
constexpr int kTwist64[12] = {0, 0, 0, 2, 2, 2, 3, 3, 3, 6, 7, 7};

// EN 302 755 表 8：解多工，輸入的第 i 個位元放到輸出的第 kDemux[i] 個位置
// （碼率 1/2、2/3、3/4 的短碼都用這一組）。
constexpr int kDemux16[8] = {7, 1, 4, 2, 5, 3, 6, 0};
constexpr int kDemux64[12] = {11, 7, 3, 10, 6, 2, 9, 5, 1, 8, 4, 0};

}  // namespace

BitInterleaver::BitInterleaver(int bits_per_cell, int k, int q) : perm_(kLdpcN) {
    std::iota(perm_.begin(), perm_.end(), std::uint16_t{0});
    if (bits_per_cell == 2) return;
    assert(bits_per_cell == 4 || bits_per_cell == 6);

    // 1. parity 交錯
    std::vector<std::uint16_t> u(kLdpcN);
    for (int i = 0; i < k; ++i) u[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(i);
    for (int s = 0; s < kLdpcGroup; ++s) {
        for (int t = 0; t < q; ++t) {
            u[static_cast<std::size_t>(k + kLdpcGroup * t + s)] = static_cast<std::uint16_t>(k + q * s + t);
        }
    }

    // 2. column-twist
    const int columns = 2 * bits_per_cell;
    const int rows = kLdpcN / columns;
    const int* twist = bits_per_cell == 4 ? kTwist16 : kTwist64;
    std::vector<std::uint16_t> v(kLdpcN);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const int src_row = ((r - twist[c]) % rows + rows) % rows;
            v[static_cast<std::size_t>(r * columns + c)] = u[static_cast<std::size_t>(c * rows + src_row)];
        }
    }

    // 3. 解多工
    const int* demux = bits_per_cell == 4 ? kDemux16 : kDemux64;
    for (int d = 0; d < rows; ++d) {
        for (int i = 0; i < columns; ++i) {
            perm_[static_cast<std::size_t>(d * columns + demux[i])] = v[static_cast<std::size_t>(d * columns + i)];
        }
    }
}

void BitInterleaver::interleave(std::span<const std::uint8_t> codeword, std::span<std::uint8_t> out) const {
    assert(codeword.size() == perm_.size() && out.size() == perm_.size());
    for (std::size_t i = 0; i < perm_.size(); ++i) out[i] = codeword[perm_[i]];
}

void BitInterleaver::deinterleave(std::span<const float> llr, std::span<float> codeword_llr) const {
    assert(llr.size() == perm_.size() && codeword_llr.size() == perm_.size());
    for (std::size_t i = 0; i < perm_.size(); ++i) codeword_llr[perm_[i]] = llr[i];
}

}  // namespace nbtv
