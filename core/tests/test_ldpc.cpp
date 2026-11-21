// LDPC：位址表的完整性、編碼器、解碼器。
#include <cmath>
#include <vector>

#include "check.hpp"
#include "ldpc.hpp"
#include "modes.hpp"
#include "rng.hpp"

using namespace nbtv;

namespace {

std::uint32_t fnv1a_u16(std::span<const std::uint16_t> values) {
    std::uint32_t h = 0x811C9DC5u;
    for (std::uint16_t v : values) {
        for (std::uint8_t byte : {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>(v >> 8)}) {
            h ^= byte;
            h *= 0x01000193u;
        }
    }
    return h;
}

// 位址表與 GNU Radio gr-dtv、xdsopl/LDPC 兩份獨立抄錄的雜湊值相同（2026-11 比對）。
void test_tables() {
    struct Expect {
        int num, den, k, count;
        long long sum;
        std::uint32_t fnv;
    };
    const Expect expect[] = {
        {1, 4, 3240, 63, 433261, 0x2A2148D2u},
        {1, 2, 7200, 85, 250978, 0xAF350D2Du},
        {2, 3, 10800, 120, 206325, 0x43471494u},
        {3, 4, 11880, 108, 159268, 0xDDC33AB1u},
    };
    for (const Expect& e : expect) {
        const LdpcTable* t = find_ldpc_table(e.num, e.den);
        CHECK(t != nullptr);
        if (t == nullptr) continue;
        CHECK_EQ(t->k, e.k);
        CHECK_EQ(static_cast<int>(t->row_degree.size()), e.k / kLdpcGroup);
        CHECK_EQ(static_cast<int>(t->address.size()), e.count);
        long long sum = 0;
        int degree_total = 0;
        for (std::uint16_t a : t->address) {
            sum += a;
            CHECK(a < kLdpcN - e.k);
        }
        for (std::uint8_t d : t->row_degree) degree_total += d;
        CHECK_EQ(degree_total, e.count);
        CHECK_EQ(sum, e.sum);
        CHECK_EQ(fnv1a_u16(t->address), e.fnv);
    }
    CHECK(find_ldpc_table(5, 6) == nullptr);
    // 模式表用到的碼率都有對應的碼，K 也一致
    for (const Mode& m : kModes) {
        if (!m.is_digital()) continue;
        const LdpcCode* code = LdpcCode::get(m.rate_num, m.rate_den);
        CHECK(code != nullptr);
        if (code) CHECK_EQ(code->k(), m.ldpc_info_bits);
    }
}

std::vector<std::uint8_t> random_bits(Rng& rng, int n) {
    std::vector<std::uint8_t> bits(static_cast<std::size_t>(n));
    for (auto& b : bits) b = static_cast<std::uint8_t>(rng.bit());
    return bits;
}

void test_encoder() {
    Rng rng(11);
    for (const LdpcTable& t : kLdpcTables) {
        const LdpcCode* code = LdpcCode::get(t.rate_num, t.rate_den);
        CHECK_EQ(code->k() + code->m(), kLdpcN);
        CHECK_EQ(code->q() * kLdpcGroup, code->m());
        const auto info = random_bits(rng, code->k());
        std::vector<std::uint8_t> cw(kLdpcN);
        code->encode(info, cw);
        CHECK(std::equal(info.begin(), info.end(), cw.begin()));  // 系統碼
        CHECK(code->check(cw));
        // 任意翻一個位元就不再是碼字
        for (int pos : {0, code->k() - 1, code->k(), kLdpcN - 1, 7777}) {
            cw[static_cast<std::size_t>(pos)] ^= 1;
            CHECK(!code->check(cw));
            cw[static_cast<std::size_t>(pos)] ^= 1;
        }
        // 線性：兩個碼字的互斥或仍是碼字
        const auto info2 = random_bits(rng, code->k());
        std::vector<std::uint8_t> cw2(kLdpcN);
        code->encode(info2, cw2);
        for (int i = 0; i < kLdpcN; ++i) cw2[static_cast<std::size_t>(i)] ^= cw[static_cast<std::size_t>(i)];
        CHECK(code->check(cw2));
        // 全 0 的資訊位元得到全 0 的碼字
        std::vector<std::uint8_t> zero(static_cast<std::size_t>(code->k()), 0);
        code->encode(zero, cw2);
        int ones = 0;
        for (std::uint8_t b : cw2) ones += b;
        CHECK_EQ(ones, 0);
    }
}

// 以 BPSK 過 AWGN：es_n0_db 是「QPSK 符號能量對 N0」，每個位元的能量是它的一半。
int decode_trials(const LdpcCode& code, double es_n0_db, int trials, Rng& rng, int* max_iterations) {
    const double ec_n0 = std::pow(10.0, es_n0_db / 10.0) / 2.0;
    const double sigma = std::sqrt(1.0 / (2.0 * ec_n0));
    int ok = 0;
    *max_iterations = 0;
    for (int trial = 0; trial < trials; ++trial) {
        const auto info = random_bits(rng, code.k());
        std::vector<std::uint8_t> cw(kLdpcN);
        code.encode(info, cw);
        std::vector<float> llr(kLdpcN);
        for (int i = 0; i < kLdpcN; ++i) {
            const double y = (cw[static_cast<std::size_t>(i)] ? -1.0 : 1.0) + sigma * rng.gauss();
            llr[static_cast<std::size_t>(i)] = static_cast<float>(2.0 * y / (sigma * sigma));
        }
        std::vector<std::uint8_t> out(kLdpcN);
        const auto r = code.decode(llr, out);
        *max_iterations = std::max(*max_iterations, r.iterations);
        if (r.converged && out == cw) ++ok;
    }
    return ok;
}

void test_decoder() {
    Rng rng(2026);
    // 沒有雜訊：不需要疊代
    {
        const LdpcCode* code = LdpcCode::get(1, 2);
        const auto info = random_bits(rng, code->k());
        std::vector<std::uint8_t> cw(kLdpcN), out(kLdpcN);
        code->encode(info, cw);
        std::vector<float> llr(kLdpcN);
        for (int i = 0; i < kLdpcN; ++i) llr[static_cast<std::size_t>(i)] = cw[static_cast<std::size_t>(i)] ? -8.0f : 8.0f;
        const auto r = code->decode(llr, out);
        CHECK(r.converged);
        CHECK_EQ(r.iterations, 0);
        CHECK(out == cw);
        // 抹掉一段（LLR = 0）仍能還原
        for (int i = 3000; i < 4500; ++i) llr[static_cast<std::size_t>(i)] = 0.0f;
        for (int i = 9000; i < 9600; ++i) llr[static_cast<std::size_t>(i)] = 0.0f;
        const auto r2 = code->decode(llr, out);
        CHECK(r2.converged);
        CHECK(out == cw);
    }
    // 比 DVB-S2 公布的門檻（QPSK，正常幀：1/4 −2.35、1/2 1.00、2/3 3.10、3/4 4.03 dB）
    // 高約 1 dB 的地方應該每幀都解得出來。
    struct Point {
        int num, den;
        double es_n0_db;
    };
    for (const Point& p : {Point{1, 4, -1.3}, Point{1, 2, 1.8}, Point{2, 3, 4.0}, Point{3, 4, 5.0}}) {
        const LdpcCode* code = LdpcCode::get(p.num, p.den);
        int iters = 0;
        const int ok = decode_trials(*code, p.es_n0_db, 4, rng, &iters);
        CHECK_EQ(ok, 4);
        std::printf("  LDPC %d/%d @ Es/N0 %.1f dB：%d/4 幀正確，最多 %d 次疊代\n", p.num, p.den, p.es_n0_db, ok, iters);
    }
    // 回歸測試：多數位元的 LLR 很大、少數位元很小且有錯（多階 QAM 在中等訊雜比的情形）。
    // 後驗值若被截得太緊，這種輸入反而解不出來。
    {
        const LdpcCode* code = LdpcCode::get(1, 2);
        int ok = 0;
        for (int trial = 0; trial < 10; ++trial) {
            const auto info = random_bits(rng, code->k());
            std::vector<std::uint8_t> cw(kLdpcN), out(kLdpcN);
            code->encode(info, cw);
            std::vector<float> llr(kLdpcN);
            for (int i = 0; i < kLdpcN; ++i) {
                const double sign = cw[static_cast<std::size_t>(i)] ? -1.0 : 1.0;
                // 一半的位元非常可靠，另一半相當於 BPSK 在 Es/N0 = 1.5 dB（約 5% 判錯）
                const double sigma = 0.6;
                llr[static_cast<std::size_t>(i)] = (i % 2 == 0) ? static_cast<float>(60.0 * sign)
                                                                 : static_cast<float>(2.0 * (sign + sigma * rng.gauss()) / (sigma * sigma));
            }
            const auto r = code->decode(llr, out);
            if (r.converged && out == cw) ++ok;
        }
        CHECK_EQ(ok, 10);
    }
    // 遠低於門檻：解不出來，但要正常結束並回報未收斂
    {
        const LdpcCode* code = LdpcCode::get(3, 4);
        int iters = 0;
        const int ok = decode_trials(*code, 1.0, 1, rng, &iters);
        CHECK_EQ(ok, 0);
        CHECK_EQ(iters, 60);
    }
}

}  // namespace

int main() {
    test_tables();
    test_encoder();
    test_decoder();
    return check::finish("ldpc");
}
