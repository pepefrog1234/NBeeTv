// 接收端合併最近幾幀：解析權重檔，以及那個小網路的前向計算。
// 和 tools/learned/merge.py 的 Gate、fuse 一一對應；改了那邊，這裡和權重檔的結構編號都要跟著改。
#include "learned_merge.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "learned_nn.hpp"

namespace nbtv {

using namespace nn;

namespace {

constexpr char kMagic[8] = {'N', 'B', 'T', 'V', 'M', 'R', 'G', '1'};
constexpr std::uint32_t kFormatVersion = 1;
// 結構編號（標頭的第二個數）：1 是「先估計差距、再決定閘」，沒收到的幀以旗標表示
constexpr std::uint32_t kStructure = 1;

constexpr int kGrid = kGridH * kGridW;  // 108 個位置
constexpr int kPooled = 6;              // 每個前面的幀、每個位置合起來算的量有幾個
constexpr int kRuleInputs = 4;
constexpr int kMaxFrames = 16;
constexpr int kMaxLayers = 16;
constexpr int kMaxRuleWidth = 256;

// 把大小差很多的數壓到差不多的範圍，保留正負
inline float squash(float v) { return std::asinh(v / 0.02f) / 4.0f; }

}  // namespace

struct LearnedMerge::Impl {
    std::uint32_t id = 0;
    std::uint32_t model_id = 0;
    int latent = 0;
    int frames = 0;
    int width = 0;

    // 估計差距的網路：幾層 3×3 的卷積（各接 GELU），最後一層 1×1，輸出是每個前面的幀一個通道
    std::vector<Conv> layers;
    Conv last;
    // 決定閘的小網路：4 個輸入 → 兩層 → 1 個輸出，加在簡單的規則上
    Linear rule1, rule2, rule3;
};

namespace {

bool build(std::span<const std::uint8_t> file, LearnedMerge::Impl& m, std::string& error) {
    std::map<std::string, RawTensor> tensors;
    FileHeader header;
    const auto accept = [](const FileHeader& h, std::string& why) {
        if (h.version != kFormatVersion || h.structure != kStructure) {
            why = "權重檔的格式版本或結構不是這個版本認得的";
            return false;
        }
        return true;
    };
    if (!parse_file(file, kMagic, "不是合併最近幾幀用的權重檔", accept, tensors, header, m.id, error)) return false;
    m.latent = static_cast<int>(header.latent);
    m.model_id = header.extra;

    // 寬度與幀數不另外記在檔案裡：由第一層卷積的形狀看出來。
    // 輸入的通道數 =（2 × 潛在通道 + kPooled + 1）×（幀數 − 1）+ 潛在通道
    const auto first = tensors.find("estimator.0.weight");
    if (first == tensors.end() || first->second.dims.size() != 4 || first->second.dims[0] > kMaxWidth) {
        error = "權重檔缺少張量或形狀不符：estimator.0.weight";
        return false;
    }
    m.width = static_cast<int>(first->second.dims[0]);
    const int inputs = static_cast<int>(first->second.dims[1]);
    const int per_frame = 2 * m.latent + kPooled + 1;
    const int earlier = (inputs - m.latent) / per_frame;
    if (inputs <= m.latent || (inputs - m.latent) % per_frame != 0 || earlier < 1 || earlier >= kMaxFrames) {
        error = "權重檔缺少張量或形狀不符：estimator.0.weight";
        return false;
    }
    m.frames = earlier + 1;

    // tools/learned/merge.py 的 estimator 是 [卷積、GELU] 重複幾次，最後接一個 1×1 的卷積：編號 0、2、4 …
    Builder b{tensors, error};
    bool ok = true;
    int index = 0;
    for (; ok && index < kMaxLayers; ++index) {
        const std::string name = "estimator." + std::to_string(2 * index);
        const auto it = tensors.find(name + ".weight");
        if (it == tensors.end() || it->second.dims.size() != 4) {
            error = "權重檔缺少張量或形狀不符：" + name + ".weight";
            return false;
        }
        if (it->second.dims[2] == 1) {
            ok = index > 0 && b.conv(name, earlier, m.width, 1, 1, 0, m.last);
            break;
        }
        m.layers.emplace_back();
        ok = b.conv(name, m.width, index == 0 ? inputs : m.width, 3, 1, 1, m.layers.back());
    }
    if (ok && index >= kMaxLayers) {
        error = "權重檔的結構不合理";
        return false;
    }
    const auto rule = tensors.find("rule.0.weight");
    if (ok && (rule == tensors.end() || rule->second.dims.size() != 2 || rule->second.dims[0] > kMaxRuleWidth)) {
        error = "權重檔缺少張量或形狀不符：rule.0.weight";
        return false;
    }
    if (ok) {
        const int rule_width = static_cast<int>(rule->second.dims[0]);
        ok = b.linear("rule.0", rule_width, kRuleInputs, m.rule1) && b.linear("rule.2", rule_width, rule_width, m.rule2) &&
             b.linear("rule.4", 1, rule_width, m.rule3);
    }
    if (!ok) return false;
    if (b.used != tensors.size()) {
        error = "權重檔裡有用不到的張量";
        return false;
    }
    return true;
}

}  // namespace

LearnedMerge::LearnedMerge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LearnedMerge::~LearnedMerge() = default;

std::unique_ptr<LearnedMerge> LearnedMerge::load(std::span<const std::uint8_t> file, std::string* error) {
    auto impl = std::make_unique<Impl>();
    std::string why;
    if (!build(file, *impl, why)) {
        if (error != nullptr) *error = why;
        return nullptr;
    }
    return std::unique_ptr<LearnedMerge>(new LearnedMerge(std::move(impl)));
}

std::uint32_t LearnedMerge::merge_id() const { return impl_->id; }
std::uint32_t LearnedMerge::model_id() const { return impl_->model_id; }
int LearnedMerge::value_count() const { return impl_->latent * kGrid; }
int LearnedMerge::frames() const { return impl_->frames; }

bool LearnedMerge::merge(std::span<const float> values, std::span<const float> noise_var, std::span<const Earlier> earlier,
                         std::span<float> merged_values, std::span<float> merged_noise_var) const {
    const Impl& m = *impl_;
    const std::size_t n = static_cast<std::size_t>(value_count());
    if (values.size() != n || noise_var.size() != n || merged_values.size() != n || merged_noise_var.size() != n) return false;
    const int latent = m.latent;
    const int slots = m.frames - 1;

    // 各個值的雜訊變異數要是正的有限數；這一幀本身不合理的話就不合併
    const auto usable = [n](std::span<const float> v, std::span<const float> s) {
        if (v.size() != n || s.size() != n) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (!std::isfinite(v[i]) || !(s[i] > 0.0f) || !std::isfinite(s[i])) return false;
        return true;
    };
    std::copy(values.begin(), values.end(), merged_values.begin());
    std::copy(noise_var.begin(), noise_var.end(), merged_noise_var.begin());
    if (!usable(values, noise_var)) return true;
    std::vector<char> have(static_cast<std::size_t>(slots), 0);
    bool any = false;
    for (int j = 0; j < slots && j < static_cast<int>(earlier.size()); ++j) {
        have[static_cast<std::size_t>(j)] = usable(earlier[static_cast<std::size_t>(j)].values, earlier[static_cast<std::size_t>(j)].noise_var);
        any = any || have[static_cast<std::size_t>(j)] != 0;
    }
    if (!any) return true;

    // ── 估計差距的網路的輸入（和 merge.py 的 Gate.estimate 相同的順序）──
    // 每個前面的幀：[log1p(差的平方 ÷ 雜訊) × 潛在通道、−log10(那一幀的雜訊)/3 × 潛在通道、合起來的 6 個量、有沒有這一幀]；
    // 最後接上這一幀的 −log10(雜訊)/3 × 潛在通道。沒收到的幀整段是 0。
    const int per_frame = 2 * latent + kPooled + 1;
    Map feats;
    feats.shape(kGridH, kGridW, per_frame * slots + latent);
    std::vector<float> mean_d2(kGrid), mean_noise(kGrid), excess(kGrid);
    for (int j = 0; j < slots; ++j) {
        if (have[static_cast<std::size_t>(j)] == 0) continue;
        const std::span<const float> yj = earlier[static_cast<std::size_t>(j)].values;
        const std::span<const float> vj = earlier[static_cast<std::size_t>(j)].noise_var;
        const int base = per_frame * j;
        for (int p = 0; p < kGrid; ++p) {
            float* f = feats.v.data() + static_cast<std::size_t>(p) * static_cast<std::size_t>(feats.c) + base;
            float sum_d2 = 0.0f, sum_noise = 0.0f;
            for (int c = 0; c < latent; ++c) {
                const std::size_t i = static_cast<std::size_t>(c) * kGrid + static_cast<std::size_t>(p);
                const float d = values[i] - yj[i];
                const float d2 = d * d;
                const float noise = 0.5f * (noise_var[i] + vj[i]);  // 兩邊雜訊變異數的和（每個實數是 σ²/2）
                f[c] = std::log1p(d2 / noise);
                f[latent + c] = -std::log10(vj[i]) / 3.0f;
                sum_d2 += d2;
                sum_noise += noise;
            }
            mean_d2[static_cast<std::size_t>(p)] = sum_d2 / static_cast<float>(latent);
            mean_noise[static_cast<std::size_t>(p)] = sum_noise / static_cast<float>(latent);
            // 這個位置的值差了多少的估計：平均起來是對的，但很吵
            excess[static_cast<std::size_t>(p)] = mean_d2[static_cast<std::size_t>(p)] - mean_noise[static_cast<std::size_t>(p)];
        }
        float excess_all = 0.0f, noise_all = 0.0f;
        for (int p = 0; p < kGrid; ++p) {
            excess_all += excess[static_cast<std::size_t>(p)];
            noise_all += mean_noise[static_cast<std::size_t>(p)];
        }
        excess_all /= static_cast<float>(kGrid);
        noise_all /= static_cast<float>(kGrid);
        for (int y = 0; y < kGridH; ++y)
            for (int x = 0; x < kGridW; ++x) {
                const int p = y * kGridW + x;
                // 周圍 3×3 的平均；邊上只算格子裡面的
                float box = 0.0f;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int yy = y + dy, xx = x + dx;
                        if (yy < 0 || yy >= kGridH || xx < 0 || xx >= kGridW) continue;
                        box += excess[static_cast<std::size_t>(yy * kGridW + xx)];
                        ++count;
                    }
                box /= static_cast<float>(count);
                float* f = feats.at(y, x) + base + 2 * latent;
                f[0] = std::log10(mean_d2[static_cast<std::size_t>(p)] + 1e-4f);
                f[1] = std::log10(mean_noise[static_cast<std::size_t>(p)]);
                f[2] = squash(excess[static_cast<std::size_t>(p)]);
                f[3] = squash(box);
                f[4] = squash(excess_all);
                f[5] = std::log10(noise_all);
                f[6] = 1.0f;
            }
    }
    for (int p = 0; p < kGrid; ++p) {
        float* f = feats.v.data() + static_cast<std::size_t>(p) * static_cast<std::size_t>(feats.c) + per_frame * slots;
        for (int c = 0; c < latent; ++c) f[c] = -std::log10(noise_var[static_cast<std::size_t>(c) * kGrid + static_cast<std::size_t>(p)]) / 3.0f;
    }

    // ── 估計：每個位置、每個前面的幀一個數，log10（差距平方的平均）──
    Map a, t;
    conv2d(m.layers[0], feats, a);
    gelu_inplace(a.v);
    for (std::size_t i = 1; i < m.layers.size(); ++i) {
        conv2d(m.layers[i], a, t);
        gelu_inplace(t.v);
        std::swap(a, t);
    }
    Map guess;
    conv2d(m.last, a, guess);
    for (float& g : guess.v) g = std::clamp(g, -4.0f, 0.7f);
    std::vector<float> guess_all(static_cast<std::size_t>(slots), 0.0f);
    for (int j = 0; j < slots; ++j) {
        float sum = 0.0f;
        for (int p = 0; p < kGrid; ++p) sum += guess.v[static_cast<std::size_t>(p) * static_cast<std::size_t>(slots) + static_cast<std::size_t>(j)];
        guess_all[static_cast<std::size_t>(j)] = sum / static_cast<float>(kGrid);
    }

    // ── 閘與合併 ──
    // 閘 = sigmoid(簡單的規則 + 小網路的修正)；簡單的規則是 雜訊 ÷（雜訊 + 8 × 差距），寫成對數就是下面的 simple。
    // 合併：照雜訊變異數加權平均，前面幾幀的權重乘上閘；合併後的雜訊變異數是 Σ 權重² × 變異數 ÷（Σ 權重）²。
    const float ln10 = 2.302585092994046f;
    const float log10_8 = 0.9030899869919435f;
    const std::size_t rule_width = static_cast<std::size_t>(m.rule1.out);
    std::vector<float> h1(rule_width), h2(rule_width);
    for (int c = 0; c < latent; ++c)
        for (int p = 0; p < kGrid; ++p) {
            const std::size_t i = static_cast<std::size_t>(c) * kGrid + static_cast<std::size_t>(p);
            const float w0 = 1.0f / noise_var[i];
            const float l0 = std::log10(noise_var[i] / 2.0f);
            float total = w0, sum = w0 * values[i], spread = w0;
            for (int j = 0; j < slots; ++j) {
                if (have[static_cast<std::size_t>(j)] == 0) continue;
                const float vj = earlier[static_cast<std::size_t>(j)].noise_var[i];
                const float ls = std::log10(vj / 2.0f);
                const float ld = guess.v[static_cast<std::size_t>(p) * static_cast<std::size_t>(slots) + static_cast<std::size_t>(j)];
                const float in[kRuleInputs] = {ld / 2.0f, ls / 2.0f, l0 / 2.0f, guess_all[static_cast<std::size_t>(j)] / 2.0f};
                for (std::size_t o = 0; o < rule_width; ++o) {
                    const float* w = m.rule1.w.data() + o * kRuleInputs;
                    h1[o] = gelu(m.rule1.b[o] + w[0] * in[0] + w[1] * in[1] + w[2] * in[2] + w[3] * in[3]);
                }
                for (std::size_t o = 0; o < rule_width; ++o) {
                    const float* w = m.rule2.w.data() + o * rule_width;
                    float s = m.rule2.b[o];
                    for (std::size_t k = 0; k < rule_width; ++k) s += w[k] * h1[k];
                    h2[o] = gelu(s);
                }
                float correction = m.rule3.b[0];
                for (std::size_t k = 0; k < rule_width; ++k) correction += m.rule3.w[k] * h2[k];
                const float simple = ln10 * (ls - ld - log10_8);
                const float gate = 1.0f / (1.0f + std::exp(-(simple + correction)));
                if (!(gate > 0.0f)) continue;
                const float wj = gate / vj;
                total += wj;
                sum += wj * earlier[static_cast<std::size_t>(j)].values[i];
                spread += gate * wj;
            }
            merged_values[i] = sum / total;
            merged_noise_var[i] = spread / (total * total);
        }
    return true;
}

}  // namespace nbtv
