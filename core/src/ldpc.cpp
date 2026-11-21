#include "ldpc.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace nbtv {

namespace {

std::vector<LdpcCode> build_all() {
    std::vector<LdpcCode> codes;
    for (const LdpcTable& t : kLdpcTables) codes.emplace_back(t);
    return codes;
}

}  // namespace

const LdpcCode* LdpcCode::get(int rate_num, int rate_den) {
    static const std::vector<LdpcCode> codes = build_all();
    std::size_t i = 0;
    for (const LdpcTable& t : kLdpcTables) {
        if (t.rate_num == rate_num && t.rate_den == rate_den) return &codes[i];
        ++i;
    }
    return nullptr;
}

LdpcCode::LdpcCode(const LdpcTable& table)
    : k_(table.k),
      m_(kLdpcN - table.k),
      q_((kLdpcN - table.k) / kLdpcGroup),
      table_addr_(table.address.begin(), table.address.end()),
      table_deg_(table.row_degree.begin(), table.row_degree.end()) {
    assert(k_ % kLdpcGroup == 0 && m_ % kLdpcGroup == 0);
    assert(static_cast<int>(table_deg_.size()) == k_ / kLdpcGroup);

    // 先數每個校驗式連到幾個資訊位元，再填入鄰接表。
    std::vector<std::uint32_t> count(static_cast<std::size_t>(m_), 0);
    auto for_each_edge = [&](auto&& fn) {
        std::size_t a = 0;
        for (std::size_t g = 0; g < table_deg_.size(); ++g) {
            for (int d = 0; d < table_deg_[g]; ++d, ++a) {
                for (int r = 0; r < kLdpcGroup; ++r) {
                    const int check = (table_addr_[a] + r * q_) % m_;
                    fn(check, static_cast<int>(g) * kLdpcGroup + r);
                }
            }
        }
    };
    for_each_edge([&](int check, int) { ++count[static_cast<std::size_t>(check)]; });

    row_start_.assign(static_cast<std::size_t>(m_) + 1, 0);
    for (int c = 0; c < m_; ++c) {
        const std::uint32_t parity_edges = c == 0 ? 1u : 2u;
        row_start_[static_cast<std::size_t>(c) + 1] =
            row_start_[static_cast<std::size_t>(c)] + count[static_cast<std::size_t>(c)] + parity_edges;
    }
    col_.assign(row_start_.back(), 0);
    std::vector<std::uint32_t> fill(row_start_.begin(), row_start_.end() - 1);
    for_each_edge([&](int check, int var) {
        col_[fill[static_cast<std::size_t>(check)]++] = static_cast<std::uint16_t>(var);
    });
    // 累加器：第 c 個校驗式連到校驗位元 c，以及（c > 0 時）c−1。
    for (int c = 0; c < m_; ++c) {
        std::uint32_t& f = fill[static_cast<std::size_t>(c)];
        if (c > 0) col_[f++] = static_cast<std::uint16_t>(k_ + c - 1);
        col_[f++] = static_cast<std::uint16_t>(k_ + c);
        assert(f == row_start_[static_cast<std::size_t>(c) + 1]);
    }
}

void LdpcCode::encode(std::span<const std::uint8_t> info, std::span<std::uint8_t> codeword) const {
    assert(static_cast<int>(info.size()) == k_ && static_cast<int>(codeword.size()) == kLdpcN);
    std::copy(info.begin(), info.end(), codeword.begin());
    std::uint8_t* parity = codeword.data() + k_;
    std::fill(parity, parity + m_, std::uint8_t{0});
    std::size_t a = 0;
    for (std::size_t g = 0; g < table_deg_.size(); ++g) {
        for (int d = 0; d < table_deg_[g]; ++d, ++a) {
            const int addr = table_addr_[a];
            for (int r = 0; r < kLdpcGroup; ++r) {
                parity[(addr + r * q_) % m_] ^= info[g * kLdpcGroup + static_cast<std::size_t>(r)];
            }
        }
    }
    for (int j = 1; j < m_; ++j) parity[j] ^= parity[j - 1];
}

bool LdpcCode::check(std::span<const std::uint8_t> codeword) const {
    assert(static_cast<int>(codeword.size()) == kLdpcN);
    for (int c = 0; c < m_; ++c) {
        unsigned parity = 0;
        for (std::uint32_t e = row_start_[static_cast<std::size_t>(c)]; e < row_start_[static_cast<std::size_t>(c) + 1]; ++e) {
            parity ^= codeword[col_[e]];
        }
        if (parity & 1u) return false;
    }
    return true;
}

LdpcCode::Result LdpcCode::decode(std::span<const float> llr, std::span<std::uint8_t> bits, int max_iterations) const {
    assert(static_cast<int>(llr.size()) == kLdpcN && static_cast<int>(bits.size()) == kLdpcN);
    constexpr int kMaxDegree = 64;
    constexpr double kInputLimit = 40.0;           // 輸入 LLR 的上限
    constexpr double kTanhLimit = 1.0 - 1e-12;     // 避免 atanh(±1)；單一訊息的大小因此不超過 28.3
    // 後驗值只做寬鬆的限制。若把它截到跟輸入一樣小，下一個校驗式扣掉自己上一輪的訊息時
    // 會得到過小的外部資訊，在多數位元都很可靠、少數位元出錯的情況（多階 QAM 的中等訊雜比）反而解不出來。
    constexpr double kPosteriorLimit = 1000.0;

    std::vector<double> post(llr.begin(), llr.end());
    for (double& v : post) v = std::clamp(v, -kInputLimit, kInputLimit);
    std::vector<float> msg(col_.size(), 0.0f);  // 校驗式 → 變數的訊息

    Result result;
    double t[kMaxDegree];
    double fwd[kMaxDegree + 1];
    double ext[kMaxDegree];

    auto hard_decision_ok = [&]() {
        for (int v = 0; v < kLdpcN; ++v) bits[static_cast<std::size_t>(v)] = post[static_cast<std::size_t>(v)] < 0.0 ? 1 : 0;
        return check(bits);
    };

    if (hard_decision_ok()) {
        result.converged = true;
        return result;
    }

    for (int iter = 1; iter <= max_iterations; ++iter) {
        for (int c = 0; c < m_; ++c) {
            const std::uint32_t begin = row_start_[static_cast<std::size_t>(c)];
            const int deg = static_cast<int>(row_start_[static_cast<std::size_t>(c) + 1] - begin);
            assert(deg <= kMaxDegree);
            // 變數 → 校驗式的訊息（扣掉這個校驗式上一輪給的）
            for (int i = 0; i < deg; ++i) {
                const double v2c = post[col_[begin + static_cast<std::uint32_t>(i)]] - msg[begin + static_cast<std::uint32_t>(i)];
                ext[i] = v2c;
                t[i] = std::tanh(0.5 * v2c);
            }
            // 排除自己的連乘：前綴積 × 後綴積
            fwd[0] = 1.0;
            for (int i = 0; i < deg; ++i) fwd[i + 1] = fwd[i] * t[i];
            double back = 1.0;
            for (int i = deg - 1; i >= 0; --i) {
                const double prod = std::clamp(fwd[i] * back, -kTanhLimit, kTanhLimit);
                back *= t[i];
                const double c2v = std::log((1.0 + prod) / (1.0 - prod));  // 2·atanh(prod)
                const std::uint32_t e = begin + static_cast<std::uint32_t>(i);
                msg[e] = static_cast<float>(c2v);
                post[col_[e]] = std::clamp(ext[i] + c2v, -kPosteriorLimit, kPosteriorLimit);
            }
        }
        result.iterations = iter;
        if (hard_decision_ok()) {
            result.converged = true;
            break;
        }
    }
    return result;
}

}  // namespace nbtv
