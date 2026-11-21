// 16QAM 與 64QAM 用的位元交錯：DVB-T2（EN 302 755 第 6.1.3 節）對 16200 短碼的規定。
//   1. parity 交錯：只動校驗位元，u[K + 360t + s] = λ[K + q·s + t]
//   2. column-twist：逐行寫入 Nc 行、各行起點錯開 tc，再逐列讀出
//   3. 解多工：每 Nc 個位元依對照表重排，前半給第一個格點、後半給第二個
// QPSK 不做位元交錯。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace nbtv {

class BitInterleaver {
public:
    // bits_per_cell 是 2、4 或 6；k 是 LDPC 的資訊位元數，q = (N−K)/360。
    BitInterleaver(int bits_per_cell, int k, int q);

    // 送出的第 i 個位元取自碼字的第 permutation()[i] 個位元。
    std::span<const std::uint16_t> permutation() const { return perm_; }

    void interleave(std::span<const std::uint8_t> codeword, std::span<std::uint8_t> out) const;
    void deinterleave(std::span<const float> llr, std::span<float> codeword_llr) const;

private:
    std::vector<std::uint16_t> perm_;
};

}  // namespace nbtv
