// DVB-S2/T2 短幀 LDPC 碼的編碼與解碼。
// 碼字是系統碼：前 K 個位元是資訊位元，後 N−K 個是校驗位元（累加器結構）。
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ldpc_tables.hpp"

namespace nbtv {

class LdpcCode {
public:
    // 取得某個碼率的碼（1/4、1/2、2/3、3/4）；物件在第一次使用時建立，之後共用。沒有這個碼率時回傳 nullptr。
    static const LdpcCode* get(int rate_num, int rate_den);

    explicit LdpcCode(const LdpcTable& table);

    int n() const { return kLdpcN; }
    int k() const { return k_; }
    int m() const { return m_; }  // 校驗位元數 N−K
    int q() const { return q_; }  // (N−K)/360，位元交錯的 parity 交錯會用到

    // info 長度 K，codeword 長度 N；每個元素是一個位元。
    void encode(std::span<const std::uint8_t> info, std::span<std::uint8_t> codeword) const;

    // 碼字是否滿足所有校驗式。
    bool check(std::span<const std::uint8_t> codeword) const;

    struct Result {
        bool converged = false;  // 所有校驗式都滿足
        int iterations = 0;
    };

    // 分層排程的 sum-product 解碼。llr 長度 N，正值表示位元較可能是 0；
    // bits 長度 N，回傳硬判決（不論是否收斂）。
    Result decode(std::span<const float> llr, std::span<std::uint8_t> bits, int max_iterations = 60) const;

private:
    int k_, m_, q_;
    // 校驗式 → 變數節點的鄰接表（CSR）。每個校驗式先列資訊位元，最後是一到兩個校驗位元。
    std::vector<std::uint32_t> row_start_;  // 長度 m+1
    std::vector<std::uint16_t> col_;        // 變數節點編號
    std::vector<std::uint16_t> table_addr_; // 編碼用：位址表
    std::vector<std::uint8_t> table_deg_;
};

}  // namespace nbtv
