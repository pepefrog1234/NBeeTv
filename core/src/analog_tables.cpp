#include "analog_tables.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <numeric>

#include "dct.hpp"

namespace nbtv {

namespace {

// 整數平方根：floor(sqrt(v))
std::uint64_t isqrt(std::uint64_t v) {
    std::uint64_t r = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(v)));
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}

struct Entry {
    std::uint32_t key;      // K = W·(9u² + 16v² + 1)
    std::uint16_t index;    // 係數編號
};

// 全部 20736 個係數依 K 由小到大排好；相同時依係數編號。
const std::vector<Entry>& ordered_coefficients() {
    static const std::vector<Entry> table = [] {
        std::vector<Entry> t;
        t.reserve(kCoefficients);
        for (int p = 0; p < kPlanes; ++p) {
            const std::uint32_t weight = p == 0 ? 1u : static_cast<std::uint32_t>(kAnalogChromaWeight);
            for (int v = 0; v < kImageHeight; ++v) {
                for (int u = 0; u < kImageWidth; ++u) {
                    const std::uint32_t key = weight * static_cast<std::uint32_t>(9 * u * u + 16 * v * v + 1);
                    t.push_back({key, static_cast<std::uint16_t>(coefficient_index(p, u, v))});
                }
            }
        }
        std::sort(t.begin(), t.end(), [](const Entry& a, const Entry& b) { return a.key != b.key ? a.key < b.key : a.index < b.index; });
        return t;
    }();
    return table;
}

// 名目振幅 sqrt(λ) 的整數表示：floor(sqrt(2^52 / K))，只用整數運算，各平台結果相同。
std::uint64_t amplitude(std::uint32_t key) { return isqrt((std::uint64_t{1} << 52) / key); }

AnalogPlan build_plan(const Mode& mode) {
    assert(mode.is_analog());
    const std::vector<Entry>& table = ordered_coefficients();
    const std::uint64_t slots = static_cast<std::uint64_t>(mode.analog_values());

    std::vector<std::uint64_t> amp(table.size()), prefix(table.size() + 1, 0);
    for (std::size_t i = 0; i < table.size(); ++i) {
        amp[i] = amplitude(table[i].key);
        prefix[i + 1] = prefix[i] + amp[i];
    }
    // 份數 m_i = ceil(s_i·V/(4·Σs))：單一數值的名目均方根不超過平均的 2 倍。
    auto copies_of = [&](std::size_t i, std::size_t n) { return std::max<std::uint64_t>(1, (amp[i] * slots + 4 * prefix[n] - 1) / (4 * prefix[n])); };

    // 取份數總和不超過 V 的最大 n。振幅由大到小，只有開頭一小段的份數大於 1。
    std::size_t n = std::min<std::size_t>(slots, table.size());
    for (; n > 0; --n) {
        std::uint64_t used = n;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t m = copies_of(i, n);
            if (m <= 1) break;
            used += m - 1;
        }
        if (used <= slots) break;
    }
    assert(n > 0);

    AnalogPlan plan;
    plan.coefficients = static_cast<int>(n);
    plan.slots = static_cast<int>(slots);
    plan.index.resize(n);
    plan.copies.resize(n);
    plan.gain.resize(n);
    plan.lambda.resize(n);
    std::uint64_t used = 0;
    for (std::size_t i = 0; i < n; ++i) {
        plan.index[i] = table[i].index;
        plan.copies[i] = static_cast<std::uint16_t>(copies_of(i, n));
        used += plan.copies[i];
        const double lambda = kAnalogEnergyScale / table[i].key;
        plan.lambda[i] = static_cast<float>(lambda);
        plan.gain[i] = static_cast<float>(std::pow(lambda, -0.25));
        plan.nominal_energy += std::sqrt(lambda);
    }
    // 剩下的位置：每次多給「每一份名目功率最大」的係數一份（比較 s_i/m_i，交叉相乘避免除法）
    for (; used < slots; ++used) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < n; ++i) {
            if (amp[i] * plan.copies[best] > amp[best] * plan.copies[i]) best = i;
        }
        ++plan.copies[best];
    }
    // 位置的順序：先放每個係數的第 1 份，再放第 2 份…，同一個係數的各份因此分散在不同格點
    plan.slot_coef.reserve(slots);
    const std::uint16_t max_copies = *std::max_element(plan.copies.begin(), plan.copies.end());
    for (std::uint16_t c = 0; c < max_copies; ++c) {
        for (std::size_t i = 0; i < n; ++i) {
            if (plan.copies[i] > c) plan.slot_coef.push_back(static_cast<std::uint16_t>(i));
        }
    }
    assert(plan.slot_coef.size() == slots);
    plan.nominal_gain = std::sqrt(0.5 * static_cast<double>(slots) / plan.nominal_energy);
    return plan;
}

}  // namespace

namespace {

// 模式表裡有幾個相干類比模式（學習式模式不算，它不用係數表）
constexpr std::size_t analog_mode_count() {
    std::size_t n = 0;
    for (const Mode& m : kModes) n += m.is_analog() ? 1 : 0;
    return n;
}

}  // namespace

const AnalogPlan& analog_plan(const Mode& mode) {
    static const std::array<AnalogPlan, analog_mode_count()> plans = [] {
        std::array<AnalogPlan, analog_mode_count()> p;
        std::size_t i = 0;
        for (const Mode& m : kModes) {
            if (m.is_analog()) p[i++] = build_plan(m);
        }
        return p;
    }();
    std::size_t i = 0;
    for (const Mode& m : kModes) {
        if (!m.is_analog()) continue;
        if (m.id == mode.id) return plans[i];
        ++i;
    }
    assert(false && "不是類比模式");
    return plans[0];
}

}  // namespace nbtv
