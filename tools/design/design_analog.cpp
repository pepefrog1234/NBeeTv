// 設計工具：由一批 96×72 的影像統計 DCT 係數的平均能量，擬合成參數模型，
// 並以解析式估算相干類比模式的畫質，比較「每張圖各自最佳」「逐係數統計表」「參數模型」三種做法。
// 用法：design_analog 影像1.ppm 影像2.ppm …
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "../ppm.hpp"
#include "dct.hpp"
#include "modes.hpp"

using namespace nbtv;

namespace {

struct Coef {
    int plane, u, v;
    double r2;  // (3u)² + (4v)²：兩個方向都換成同樣的空間頻率單位（96 = 4·24、72 = 3·24）
};

std::vector<Coef> make_coefs() {
    std::vector<Coef> c(kCoefficients);
    for (int p = 0; p < kPlanes; ++p) {
        for (int v = 0; v < kImageHeight; ++v) {
            for (int u = 0; u < kImageWidth; ++u) {
                c[static_cast<std::size_t>(coefficient_index(p, u, v))] = {p, u, v, 9.0 * u * u + 16.0 * v * v};
            }
        }
    }
    return c;
}

constexpr double kPlaneWeight[3] = {1.0, 1.0862, 0.8252};  // 各平面的誤差換算成 RGB 均方誤差的權重

// 一張表（順序與先驗能量）在某個模式下的配置：傳前 n 個係數，各係數的名目振幅 s = sqrt(λ)。
struct Plan {
    std::vector<int> order;
    std::vector<double> s;  // 依傳送順序
    int n = 0;
    int slots = 0;
    double nominal = 0;     // Σ s（前 n 個）
};

// 分配：n 個係數、各 m_i = ceil(s_i·V/(4·Σs)) 份（單一數值的名目均方根不超過平均的 2 倍），
// 取份數總和不超過 V 的最大 n。s 由大到小，所以只有開頭一小段的 m_i 大於 1。
Plan make_plan(const std::vector<int>& order, const std::vector<double>& lambda, int slots) {
    Plan p;
    p.order = order;
    p.slots = slots;
    p.s.resize(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) p.s[i] = std::sqrt(lambda[static_cast<std::size_t>(order[i])]);
    std::vector<double> prefix(order.size() + 1, 0.0);
    for (std::size_t i = 0; i < order.size(); ++i) prefix[i + 1] = prefix[i] + p.s[i];
    for (int n = std::min<int>(slots, static_cast<int>(order.size())); n > 0; --n) {
        long used = n;
        for (int i = 0; i < n; ++i) {
            const long m = static_cast<long>(std::ceil(p.s[static_cast<std::size_t>(i)] * slots / (4.0 * prefix[static_cast<std::size_t>(n)])));
            if (m <= 1) break;
            used += m - 1;
        }
        if (used <= slots) {
            p.n = n;
            p.nominal = prefix[static_cast<std::size_t>(n)];
            break;
        }
    }
    return p;
}

// 解析式的畫質：image 是某張圖的係數，snr 是格點訊雜比（線性）。
double psnr_for(const Plan& p, const std::vector<double>& lambda, const std::vector<float>& image, double snr) {
    double actual = 0, total = 0;
    for (std::size_t k = 0; k < image.size(); ++k) total += kPlaneWeight[k / kImagePixels] * static_cast<double>(image[k]) * image[k];
    for (int i = 0; i < p.n; ++i) {
        const double c = image[static_cast<std::size_t>(p.order[static_cast<std::size_t>(i)])];
        actual += c * c / p.s[static_cast<std::size_t>(i)];  // g² = λ^(−1/2)
    }
    if (actual <= 0) return 99.0;
    const double g2 = 0.5 * p.slots / actual;  // 幀增益的平方
    const double kappa = actual / p.nominal;   // 這張圖的能量相對於名目值
    const double noise = 0.5 / snr;            // 每個實數的雜訊變異數
    double err = total;                        // 先當作全部沒傳，再把有傳的換成估計誤差
    for (int i = 0; i < p.n; ++i) {
        const int k = p.order[static_cast<std::size_t>(i)];
        const double c = image[static_cast<std::size_t>(k)];
        const double gain2 = g2 / p.s[static_cast<std::size_t>(i)];  // (G·g)²
        const double prior = kappa * lambda[static_cast<std::size_t>(k)];
        const double w = prior * gain2 / (prior * gain2 + noise);
        err += kPlaneWeight[k / kImagePixels] * ((1 - w) * (1 - w) * c * c + w * w * noise / gain2 - c * c);
    }
    return 10.0 * std::log10(255.0 * 255.0 / (std::max(err, 1e-9) / kImagePixels));
}

std::vector<int> order_by(const std::vector<double>& lambda) {
    std::vector<int> order(lambda.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return lambda[static_cast<std::size_t>(a)] > lambda[static_cast<std::size_t>(b)]; });
    return order;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::vector<float>> images;
    for (int a = 1; a < argc; ++a) {
        std::vector<std::uint8_t> rgb;
        int w = 0, h = 0;
        if (!tools::read_ppm(argv[a], rgb, &w, &h) || w != kImageWidth || h != kImageHeight) {
            std::fprintf(stderr, "略過 %s（需要 96×72 的 PPM）\n", argv[a]);
            continue;
        }
        std::vector<float> planes(kCoefficients), coef(kCoefficients);
        rgb_to_ycbcr(rgb, planes);
        dct_forward(planes, coef);
        images.push_back(std::move(coef));
    }
    if (images.empty()) {
        std::fprintf(stderr, "沒有影像\n");
        return 1;
    }
    const std::vector<Coef> coefs = make_coefs();
    std::vector<double> emp(kCoefficients, 0.0);
    for (const auto& img : images) {
        for (int k = 0; k < kCoefficients; ++k) emp[static_cast<std::size_t>(k)] += static_cast<double>(img[static_cast<std::size_t>(k)]) * img[static_cast<std::size_t>(k)] / images.size();
    }

    std::printf("影像 %zu 張\n\n各平面的平均能量依空間頻率分段（r = sqrt((3u)²+(4v)²)，r=288 相當於每像素半個週期）：\n", images.size());
    const double edges[] = {0, 1, 4, 8, 16, 32, 64, 128, 256, 600};
    std::printf("  %-12s %12s %12s %12s\n", "r 範圍", "Y", "Cb", "Cr");
    for (int b = 0; b + 1 < 10; ++b) {
        double sum[3] = {0, 0, 0};
        int cnt[3] = {0, 0, 0};
        for (int k = 0; k < kCoefficients; ++k) {
            const double r = std::sqrt(coefs[static_cast<std::size_t>(k)].r2);
            if (r >= edges[b] && r < edges[b + 1]) {
                sum[coefs[static_cast<std::size_t>(k)].plane] += emp[static_cast<std::size_t>(k)];
                ++cnt[coefs[static_cast<std::size_t>(k)].plane];
            }
        }
        std::printf("  [%3.0f,%3.0f)    %12.4g %12.4g %12.4g   (n=%d)\n", edges[b], edges[b + 1], sum[0] / std::max(cnt[0], 1),
                    sum[1] / std::max(cnt[1], 1), sum[2] / std::max(cnt[2], 1), cnt[0]);
    }

    // 擬合 log λ = a_p − γ·log(r² + c_p)。先各平面獨立（γ、c 用格點搜尋，a 有解析解）。
    std::printf("\n各平面獨立擬合 λ = A/(r²+c)^γ：\n");
    double best_gamma[3], best_c[3], best_a[3];
    for (int p = 0; p < 3; ++p) {
        double best = 1e300;
        for (double gamma = 0.6; gamma <= 2.4; gamma += 0.05) {
            for (double c = 1; c <= 4096; c *= 1.4142) {
                double sa = 0;
                int n = 0;
                for (int k = p * kImagePixels; k < (p + 1) * kImagePixels; ++k) {
                    sa += std::log(emp[static_cast<std::size_t>(k)]) + gamma * std::log(coefs[static_cast<std::size_t>(k)].r2 + c);
                    ++n;
                }
                const double a = sa / n;
                double e = 0;
                for (int k = p * kImagePixels; k < (p + 1) * kImagePixels; ++k) {
                    const double d = std::log(emp[static_cast<std::size_t>(k)]) - (a - gamma * std::log(coefs[static_cast<std::size_t>(k)].r2 + c));
                    e += d * d;
                }
                if (e < best) {
                    best = e;
                    best_gamma[p] = gamma;
                    best_c[p] = c;
                    best_a[p] = a;
                }
            }
        }
        std::printf("  平面 %d：γ = %.2f、c = %.0f、ln A = %.2f，對數誤差均方根 %.2f\n", p, best_gamma[p], best_c[p], best_a[p],
                    std::sqrt(best / kImagePixels));
    }

    // 候選模型：共同的 γ，λ = A/(W_p·(r² + c_p))^γ
    struct Model {
        const char* name;
        double gamma, w[3], c[3];
    };
    std::vector<Model> models;
    for (double gamma : {1.0, 1.25, 1.5, 2.0}) {
        for (double wc : {2.0, 4.0, 6.0, 8.0, 12.0, 16.0}) {
            for (double c : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 128.0}) models.push_back({"", gamma, {1.0, wc, wc}, {c, c, c}});
        }
    }

    const double snrs_db[] = {0, 10, 20, 30};
    auto report = [&](const char* name, const std::vector<int>& order, const std::vector<double>& lambda, bool print) {
        double score = 0;
        if (print) std::printf("  %-28s", name);
        for (int mode_id : {8, 9, 10}) {
            const Mode& m = *find_mode(mode_id);
            const Plan plan = make_plan(order, lambda, m.analog_values());
            for (double snr_db : snrs_db) {
                double acc = 0;
                for (const auto& img : images) acc += psnr_for(plan, lambda, img, std::pow(10.0, snr_db / 10.0));
                acc /= static_cast<double>(images.size());
                score += acc;
                if (print) std::printf(" %5.1f", acc);
            }
            if (print) std::printf(" |");
        }
        if (print) std::printf("  平均 %.2f\n", score / 12.0);
        return score / 12.0;
    };

    std::printf("\n解析式估算的平均 PSNR（dB）；每個模式四欄是格點訊雜比 0、10、20、30 dB：\n");
    std::printf("  %-28s %-24s| %-24s| %-24s|\n", "", "模式 8", "模式 9", "模式 10");
    // 每張圖各自最佳
    {
        std::printf("  %-28s", "每張圖各自最佳");
        double total = 0;
        for (int mode_id : {8, 9, 10}) {
            const Mode& m = *find_mode(mode_id);
            double acc[4] = {0, 0, 0, 0};
            for (const auto& img : images) {
                std::vector<double> lam(kCoefficients);
                for (int k = 0; k < kCoefficients; ++k) lam[static_cast<std::size_t>(k)] = std::max(1e-6, static_cast<double>(img[static_cast<std::size_t>(k)]) * img[static_cast<std::size_t>(k)]);
                const Plan plan = make_plan(order_by(lam), lam, m.analog_values());
                for (int q = 0; q < 4; ++q) acc[q] += psnr_for(plan, lam, img, std::pow(10.0, snrs_db[q] / 10.0)) / static_cast<double>(images.size());
            }
            for (int q = 0; q < 4; ++q) {
                total += acc[q];
                std::printf(" %5.1f", acc[q]);
            }
            std::printf(" |");
        }
        std::printf("  平均 %.2f\n", total / 12.0);
    }
    report("逐係數統計表（同一批圖）", order_by(emp), emp, true);

    double best_score = -1e9;
    Model best_model{};
    for (const Model& m : models) {
        std::vector<double> lam(kCoefficients);
        for (int k = 0; k < kCoefficients; ++k) {
            const Coef& c = coefs[static_cast<std::size_t>(k)];
            lam[static_cast<std::size_t>(k)] = std::pow(m.w[c.plane] * (c.r2 + m.c[c.plane]), -m.gamma);
        }
        // 整體的比例 A：讓名目總能量與統計值一致（只影響 κ，不影響順序）
        double se = 0, sm = 0;
        for (int k = 0; k < kCoefficients; ++k) {
            se += std::sqrt(emp[static_cast<std::size_t>(k)]);
            sm += std::sqrt(lam[static_cast<std::size_t>(k)]);
        }
        const double scale = (se / sm) * (se / sm);
        for (double& v : lam) v *= scale;
        const double score = report("", order_by(lam), lam, false);
        if (score > best_score) {
            best_score = score;
            best_model = m;
        }
    }
    {
        // 核心採用的模型：λ = A/K，K = W_p·((3u)² + (4v)² + 1)，W = {1, 8, 8}。A 取名目總振幅與統計值一致的值。
        double se = 0, sm = 0;
        for (int k = 0; k < kCoefficients; ++k) {
            const Coef& c = coefs[static_cast<std::size_t>(k)];
            se += std::sqrt(emp[static_cast<std::size_t>(k)]);
            sm += 1.0 / std::sqrt((c.plane == 0 ? 1.0 : 8.0) * (c.r2 + 1.0));
        }
        std::printf("\n核心模型 λ = A/(W·(r²+1))、W = {1, 8, 8}：A = %.4g（sqrt(A) = %.1f）\n", (se / sm) * (se / sm), se / sm);
    }
    std::printf("\n參數模型掃描（γ × 色度權重 × c）：最好的是 γ = %.2f、色度權重 W = %.0f、c = %.0f，平均 %.2f dB\n", best_model.gamma,
                best_model.w[1], best_model.c[0], best_score);
    for (const Model& m : models) {
        if (m.c[0] != best_model.c[0]) continue;
        std::vector<double> lam(kCoefficients);
        for (int k = 0; k < kCoefficients; ++k) {
            const Coef& c = coefs[static_cast<std::size_t>(k)];
            lam[static_cast<std::size_t>(k)] = std::pow(m.w[c.plane] * (c.r2 + m.c[c.plane]), -m.gamma);
        }
        double se = 0, sm = 0;
        for (int k = 0; k < kCoefficients; ++k) {
            se += std::sqrt(emp[static_cast<std::size_t>(k)]);
            sm += std::sqrt(lam[static_cast<std::size_t>(k)]);
        }
        const double scale = (se / sm) * (se / sm);
        for (double& v : lam) v *= scale;
        char name[64];
        std::snprintf(name, sizeof name, "γ=%.2f W=%.0f c=%.0f", m.gamma, m.w[1], m.c[0]);
        report(name, order_by(lam), lam, true);
    }
    return 0;
}
