// 可重現的亂數：測試、通道模擬與設計工具共用。
// 不用 <random> 的分布，因為各家標準函式庫的實作不同，同一個種子得不到同樣的數列。
#pragma once

#include <cmath>
#include <cstdint>

#include "types.hpp"

namespace nbtv {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 1) {
        // 以 splitmix64 把種子展開成 xoshiro256** 的狀態
        std::uint64_t z = seed;
        for (auto& s : state_) {
            z += 0x9E3779B97F4A7C15ULL;
            std::uint64_t x = z;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            s = x ^ (x >> 31);
        }
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(state_[1] * 5, 7) * 9;
        const std::uint64_t t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);
        return result;
    }

    // [0, 1) 的均勻分布
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

    // [0, n) 的整數
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(uniform() * n); }

    int bit() { return static_cast<int>(next() >> 63); }

    // 標準常態分布（Box–Muller）
    double gauss() {
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }
        double u1 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        const double u2 = uniform();
        const double r = std::sqrt(-2.0 * std::log(u1));
        spare_ = r * std::sin(kTwoPi * u2);
        has_spare_ = true;
        return r * std::cos(kTwoPi * u2);
    }

    // 平均功率為 1 的複數常態分布
    cd cgauss() {
        const double a = gauss();
        const double b = gauss();
        return cd(a, b) * 0.7071067811865476;
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    std::uint64_t state_[4];
    double spare_ = 0.0;
    bool has_spare_ = false;
};

}  // namespace nbtv
