#include "chanest.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>

#include "fft.hpp"
#include "frame_map.hpp"
#include "linalg.hpp"

namespace nbtv {

namespace {

constexpr int kEven = kS0Carriers;       // 25 個偶數格：第 8、10、…、56 格
constexpr int kTimeSpan = 10;            // 時間內插取前後各幾個符號內的已知格點
constexpr double kTimeNoise = 0.1;       // 時間內插的設計雜訊比（雜訊變異數 ÷ |H|²）
constexpr double kFreqNoise = 0.003;     // 頻率內插的設計雜訊比：取小一點，視窗內的響應誤差才壓得到 −35 dB
constexpr double kSymbolSeconds = static_cast<double>(kSymbolSamples) / kSampleRate;

double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

int bin_of_even(int i) { return kFirstBin + 2 * i; }

// 第 i 個偶數格在酬載符號 j 是導頻的條件：j mod 3 == pilot_class(i)
int pilot_class(int i) { return (bin_of_even(i) % 6) / 2; }

// 已知格點的最小平方估計，雜訊變異數相對於格點雜訊的倍數：1/|X|²
double known_noise(int row) {
    if (row == 0) return 1.0 / (static_cast<double>(kS0Amplitude) * kS0Amplitude);
    if (row == 1) return 1.0 / (static_cast<double>(kS1Amplitude) * kS1Amplitude);
    return 1.0 / (static_cast<double>(kPilotAmplitude) * kPilotAmplitude);
}

// 時間方向的通道自相關：都卜勒頻譜假設在 ±kChanestDopplerHz 內均勻
double time_corr(int row_delta) { return sinc(2.0 * kChanestDopplerHz * kSymbolSeconds * row_delta); }

const Fft& fft128() {
    static const Fft fft(kFftSize / 2);
    return fft;
}

}  // namespace

struct ChannelEstimator::TimeWeights {
    struct Entry {
        std::vector<int> rows;   // 參考的列
        std::vector<double> w;   // 權重
        double self = 0.0;       // 目標列本身（若是這一類的導頻列）的權重
        double gain = 0.0;       // Σ w²·ν：內插後的雜訊增益
    };
    int row_count = 0;
    std::vector<Entry> entries;  // row·3 + class
    const Entry& at(int row, int cls) const { return entries[static_cast<std::size_t>(row * 3 + cls)]; }
};

ChannelEstimator::ChannelEstimator() : freq_weights_(kCarriers * kEven), freq_gain_(kCarriers * 3, 0.0) {
    // 頻率方向的通道自相關：路徑延遲假設在 [−before, +after] 內均勻分布
    const double ta = -kChanestDelayBefore, tb = kChanestDelayAfter;
    auto corr = [&](int delta) {
        // sinc 可能是負的，不能直接當 std::polar 的大小
        const double angle = -kPi * delta * (ta + tb) / kFftSize;
        return sinc(delta * (tb - ta) / kFftSize) * cd(std::cos(angle), std::sin(angle));
    };
    // 對每個目標格 c 求 W_c，使 Σ_b W_c[b]·H[偶數格 b] 最接近 H[c]：
    // W_c·(R + ηI) = r_c，其中 R[a][b] = corr(2a − 2b)、r_c[b] = corr(c − 2b)。
    // 轉置後一次解 49 個右手邊。
    std::vector<cd> a(static_cast<std::size_t>(kEven * kEven));
    std::vector<cd> b(static_cast<std::size_t>(kEven * kCarriers));
    for (int r = 0; r < kEven; ++r) {
        for (int c = 0; c < kEven; ++c) {
            a[static_cast<std::size_t>(r * kEven + c)] = corr(2 * c - 2 * r) + (r == c ? kFreqNoise : 0.0);  // (R + ηI) 的轉置
        }
        for (int c = 0; c < kCarriers; ++c) b[static_cast<std::size_t>(r * kCarriers + c)] = corr(c - 2 * r);
    }
    const bool ok = solve_linear(a, b, kEven, kCarriers);
    assert(ok);
    (void)ok;
    for (int c = 0; c < kCarriers; ++c) {
        for (int i = 0; i < kEven; ++i) {
            const cd w = b[static_cast<std::size_t>(i * kCarriers + c)];
            freq_weights_[static_cast<std::size_t>(c * kEven + i)] = w;
            freq_gain_[static_cast<std::size_t>(c * 3 + pilot_class(i))] += std::norm(w);
        }
    }
}

std::vector<cf> ChannelEstimator::known_cell_estimates(const Mode& mode, const SymbolGrid& grid) const {
    std::vector<cf> ls(static_cast<std::size_t>(grid.rows) * kEven, cf(0, 0));
    for (int i = 0; i < kEven; ++i) {
        const int bin = bin_of_even(i);
        const int c = 2 * i;
        const cf s0 = s0_value(bin);
        ls[static_cast<std::size_t>(i)] = grid.at(0, c) * std::conj(s0) / std::norm(s0);
        ls[static_cast<std::size_t>(kEven + i)] = grid.at(1, c) / s1_value(mode.id, bin);
        const float pilot = pilot_value(bin);
        for (int j = pilot_class(i); j < mode.payload_symbols; j += 3) {
            const int row = j + kPreambleSymbols;
            ls[static_cast<std::size_t>(row * kEven + i)] = grid.at(row, c) / pilot;
        }
    }
    return ls;
}

double ChannelEstimator::residual_phase_per_symbol(const Mode& mode, const SymbolGrid& grid) const {
    assert(grid.rows == mode.frame_symbols());
    const std::vector<cf> ls = known_cell_estimates(mode, grid);
    cd acc(0, 0);
    for (int i = 0; i < kEven; ++i) {
        for (int j = pilot_class(i); j + 3 < mode.payload_symbols; j += 3) {
            const int row = j + kPreambleSymbols;
            acc += cd(ls[static_cast<std::size_t>((row + 3) * kEven + i)]) * std::conj(cd(ls[static_cast<std::size_t>(row * kEven + i)]));
        }
    }
    return std::abs(acc) > 0.0 ? std::arg(acc) / 3.0 : 0.0;
}

ChannelEstimator::TimeWeights ChannelEstimator::time_weights(const Mode& mode, int cut_from, int cut_to) const {
    TimeWeights tw;
    tw.row_count = mode.frame_symbols();
    tw.entries.resize(static_cast<std::size_t>(tw.row_count) * 3);
    // 遠離幀頭幀尾的列，參考格點的相對位置只有三種，解過的就重複使用。
    std::map<std::vector<int>, std::vector<double>> cache;
    for (int row = kPreambleSymbols; row < tw.row_count; ++row) {
        // 這一列可以參考的範圍：整幀；斷開過的幀（見 estimate）只參考自己那一段
        int lo = 0, hi = tw.row_count - 1;
        if (cut_from >= 0) {
            if (row < cut_from) hi = cut_from - 1;
            else lo = cut_to;
        }
        for (int cls = 0; cls < 3; ++cls) {
            TimeWeights::Entry& e = tw.entries[static_cast<std::size_t>(row * 3 + cls)];
            std::vector<int> key;
            for (int r = std::max(lo, row - kTimeSpan); r <= std::min(hi, row + kTimeSpan); ++r) {
                const bool known = r < kPreambleSymbols || (r - kPreambleSymbols) % 3 == cls;
                if (!known) continue;
                e.rows.push_back(r);
                key.push_back((r - row) + (r == 0 ? 1000 : r == 1 ? 2000 : 0));  // S0、S1 的雜訊倍數不同
            }
            const int n = static_cast<int>(e.rows.size());
            if (n == 0) continue;  // 這一段太短，沒有這一類的導頻：估計值是 0（只會發生在斷開過的幀）
            auto it = cache.find(key);
            if (it == cache.end()) {
                std::vector<double> a(static_cast<std::size_t>(n * n)), rhs(static_cast<std::size_t>(n));
                for (int p = 0; p < n; ++p) {
                    for (int q = 0; q < n; ++q) {
                        a[static_cast<std::size_t>(p * n + q)] = time_corr(e.rows[static_cast<std::size_t>(p)] - e.rows[static_cast<std::size_t>(q)]) +
                                                               (p == q ? kTimeNoise * known_noise(e.rows[static_cast<std::size_t>(p)]) : 0.0);
                    }
                    rhs[static_cast<std::size_t>(p)] = time_corr(e.rows[static_cast<std::size_t>(p)] - row);
                }
                const bool ok = solve_linear(a, rhs, n, 1);
                assert(ok);
                (void)ok;
                // Wiener 解會把估計值往 0 收縮；多階 QAM 與雜訊估計都需要不偏的振幅，
                // 所以把權重正規化成總和為 1（通道不變時估計值沒有偏差）。
                double sum = 0.0;
                for (double w : rhs) sum += w;
                for (double& w : rhs) w /= sum;
                it = cache.emplace(std::move(key), std::move(rhs)).first;
            }
            e.w = it->second;
            for (int p = 0; p < n; ++p) {
                const double w = e.w[static_cast<std::size_t>(p)];
                e.gain += w * w * known_noise(e.rows[static_cast<std::size_t>(p)]);
                if (e.rows[static_cast<std::size_t>(p)] == row) e.self = w;
            }
        }
    }
    return tw;
}

std::vector<cf> ChannelEstimator::interpolate_time(const Mode& mode, const std::vector<cf>& ls, const TimeWeights& tw) const {
    std::vector<cf> he(static_cast<std::size_t>(tw.row_count) * kEven, cf(0, 0));
    (void)mode;
    for (int row = kPreambleSymbols; row < tw.row_count; ++row) {
        for (int i = 0; i < kEven; ++i) {
            const TimeWeights::Entry& e = tw.at(row, pilot_class(i));
            cf acc(0, 0);
            for (std::size_t p = 0; p < e.rows.size(); ++p) {
                acc += ls[static_cast<std::size_t>(e.rows[p] * kEven + i)] * static_cast<float>(e.w[p]);
            }
            he[static_cast<std::size_t>(row * kEven + i)] = acc;
        }
    }
    return he;
}

double ChannelEstimator::first_path_delay(const Mode& mode, const SymbolGrid& grid) const {
    assert(grid.rows == mode.frame_symbols());
    const TimeWeights tw = time_weights(mode);
    const std::vector<cf> he = interpolate_time(mode, known_cell_estimates(mode, grid), tw);

    // 偶數格相隔 2 格，所以用 128 點反 FFT，延遲的範圍是 ±64 個取樣。
    constexpr int kN = kFftSize / 2;
    std::vector<double> profile(kN, 0.0);
    std::vector<cf> x(kN);
    const int step = std::max(1, mode.payload_symbols / 40);
    for (int row = kPreambleSymbols; row < grid.rows; row += step) {
        std::fill(x.begin(), x.end(), cf(0, 0));
        for (int i = 0; i < kEven; ++i) {
            const float hann = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * (i + 1) / (kEven + 1)));
            x[static_cast<std::size_t>(kFirstBin / 2 + i)] = he[static_cast<std::size_t>(row * kEven + i)] * hann;
        }
        fft128().inverse(x.data());
        for (int t = 0; t < kN; ++t) profile[static_cast<std::size_t>(t)] += std::norm(x[static_cast<std::size_t>(t)]);
    }
    const int peak = static_cast<int>(std::max_element(profile.begin(), profile.end()) - profile.begin());
    std::vector<double> sorted = profile;
    std::nth_element(sorted.begin(), sorted.begin() + kN / 2, sorted.end());
    const double floor = sorted[kN / 2];
    const double threshold = std::max(0.05 * profile[static_cast<std::size_t>(peak)], 5.0 * floor);
    // 從峰值往前 48 個取樣內，找最早超過門檻的位置
    int first = peak;
    for (int back = 48; back > 0; --back) {
        const int t = ((peak - back) % kN + kN) % kN;
        if (profile[static_cast<std::size_t>(t)] >= threshold) {
            first = peak - back;
            break;
        }
    }
    // 加窗後主瓣在門檻處大約提前 6 個取樣
    double delay = first + (first == peak ? 0.0 : 6.0);
    constexpr double kHalfRange = kN / 2.0;
    if (delay >= kHalfRange) delay -= kN;
    return delay;
}

ChannelEstimate ChannelEstimator::estimate(const Mode& mode, const SymbolGrid& grid, int cut_from, int cut_to) const {
    assert(grid.rows == mode.frame_symbols());
    assert(cut_from < 0 || (cut_from >= kPreambleSymbols && cut_from <= cut_to && cut_to <= grid.rows));
    const TimeWeights tw = time_weights(mode, cut_from, cut_to);
    const std::vector<cf> he = interpolate_time(mode, known_cell_estimates(mode, grid), tw);

    ChannelEstimate est;
    est.h.assign(grid.cells.size(), cf(0, 0));
    est.est_gain.assign(grid.cells.size(), 0.0f);

    // 頻率方向內插
    std::vector<cf> wf(freq_weights_.size());
    for (std::size_t n = 0; n < wf.size(); ++n) wf[n] = cf(freq_weights_[n]);
    for (int row = kPreambleSymbols; row < grid.rows; ++row) {
        const cf* in = &he[static_cast<std::size_t>(row * kEven)];
        for (int c = 0; c < kCarriers; ++c) {
            const cf* w = &wf[static_cast<std::size_t>(c * kEven)];
            cf acc(0, 0);
            for (int i = 0; i < kEven; ++i) acc += w[i] * in[i];
            est.h[static_cast<std::size_t>(row * kCarriers + c)] = acc;
            double gain = 0.0;
            for (int cls = 0; cls < 3; ++cls) gain += freq_gain_[static_cast<std::size_t>(c * 3 + cls)] * tw.at(row, cls).gain;
            est.est_gain[static_cast<std::size_t>(row * kCarriers + c)] = static_cast<float>(gain);
        }
    }

    // 雜訊變異數：導頻的殘差。導頻本身也參與了自己那一格的估計，所以殘差的期望值是
    // σ²·(1 + |X|²·G − 2·w_self)，G 是那一格的雜訊增益，w_self 是導頻自己的權重；除掉這個係數就是 σ² 的估計。
    struct Residual {
        int row, even;  // 列、第幾個偶數格
        double value;   // 殘差 ÷ 期望係數
    };
    std::vector<Residual> residuals;
    residuals.reserve(static_cast<std::size_t>(mode.payload_symbols) * 9);
    double power = 0.0, gain_sum = 0.0;
    long data_count = 0;
    const double pilot_power = static_cast<double>(kPilotAmplitude) * kPilotAmplitude;
    for (int j = 0; j < mode.payload_symbols; ++j) {
        const int row = j + kPreambleSymbols;
        for (int c = 0; c < kCarriers; ++c) {
            const int bin = kFirstBin + c;
            const cf h = est.h[static_cast<std::size_t>(row * kCarriers + c)];
            const double gain = est.est_gain[static_cast<std::size_t>(row * kCarriers + c)];
            if (is_pilot(j, bin)) {
                const int i = c / 2;
                const cf e = grid.at(row, c) - h * pilot_value(bin);
                const double w_self = freq_weights_[static_cast<std::size_t>(c * kEven + i)].real() * tw.at(row, pilot_class(i)).self;
                const double expect = std::max(1.0 + pilot_power * gain - 2.0 * w_self, 0.05);
                residuals.push_back({row, i, std::norm(e) / expect});
            } else {
                power += std::norm(h);
                gain_sum += gain;
                ++data_count;
            }
        }
    }

    // 把殘差拆成「每一列的雜訊」×「每個載波的倍數」。
    // 只有明顯偏高的列或載波才另外給值，其餘一律用同一個雜訊變異數：逐列、逐載波的估計只靠少數幾個導頻，
    // 起伏很大，全面採用的話，在一般的白雜訊下反而讓解碼門檻變差約 0.1 dB。
    auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    const int rows = grid.rows;

    // 1. 載波：以各載波殘差的中位數和全體的中位數相比（中位數不受少數幾列的突波影響）
    std::vector<double> carrier_ratio(kEven, 1.0);
    std::vector<char> carrier_bad(kEven, 0);
    {
        std::vector<std::vector<double>> per_carrier(kEven);
        std::vector<double> all;
        all.reserve(residuals.size());
        for (const Residual& r : residuals) {
            per_carrier[static_cast<std::size_t>(r.even)].push_back(r.value);
            all.push_back(r.value);
        }
        const double global = std::max(median(all), 1e-30);
        for (int i = 0; i < kEven; ++i) {
            const std::vector<double>& v = per_carrier[static_cast<std::size_t>(i)];
            if (v.size() < 4) continue;
            const double ratio = median(v) / global;
            if (ratio > 1.0 + 5.0 / std::sqrt(static_cast<double>(v.size()))) {
                carrier_bad[static_cast<std::size_t>(i)] = 1;
                carrier_ratio[static_cast<std::size_t>(i)] = ratio;
            }
        }
    }

    // 2. 列：每一列只有 8、9 個導頻，平均值超過中位數的 2.7 倍（約 5 個標準差）才算突波
    std::vector<double> row_sum(static_cast<std::size_t>(rows), 0.0), row_count(static_cast<std::size_t>(rows), 0.0);
    for (const Residual& r : residuals) {
        if (carrier_bad[static_cast<std::size_t>(r.even)]) continue;
        row_sum[static_cast<std::size_t>(r.row)] += r.value;
        row_count[static_cast<std::size_t>(r.row)] += 1.0;
    }
    std::vector<double> row_mean(static_cast<std::size_t>(rows), 0.0), payload_means;
    for (int row = kPreambleSymbols; row < rows; ++row) {
        if (row_count[static_cast<std::size_t>(row)] <= 0.0) continue;
        row_mean[static_cast<std::size_t>(row)] = row_sum[static_cast<std::size_t>(row)] / row_count[static_cast<std::size_t>(row)];
        payload_means.push_back(row_mean[static_cast<std::size_t>(row)]);
    }
    const double row_limit = 2.7 * median(payload_means);
    double base_sum = 0.0, base_count = 0.0;
    for (int row = kPreambleSymbols; row < rows; ++row) {
        if (row_mean[static_cast<std::size_t>(row)] > row_limit) continue;
        base_sum += row_sum[static_cast<std::size_t>(row)];
        base_count += row_count[static_cast<std::size_t>(row)];
    }
    // 一般情況下的雜訊變異數：沒有突波的列、沒有干擾的載波上，所有導頻殘差的平均
    const double base = std::max(base_count > 0.0 ? base_sum / base_count : median(payload_means), 1e-20);

    est.noise_var = static_cast<float>(base);
    est.row_noise.assign(static_cast<std::size_t>(rows), static_cast<float>(base));
    for (int row = kPreambleSymbols; row < rows; ++row) {
        if (row_mean[static_cast<std::size_t>(row)] > row_limit) est.row_noise[static_cast<std::size_t>(row)] = static_cast<float>(row_mean[static_cast<std::size_t>(row)]);
    }

    // 3. 有干擾的載波：倍數改用沒有突波的列上的平均值（比中位數準）
    {
        std::vector<double> sum(kEven, 0.0), count(kEven, 0.0);
        for (const Residual& r : residuals) {
            if (!carrier_bad[static_cast<std::size_t>(r.even)] || row_mean[static_cast<std::size_t>(r.row)] > row_limit) continue;
            sum[static_cast<std::size_t>(r.even)] += r.value;
            count[static_cast<std::size_t>(r.even)] += 1.0;
        }
        for (int i = 0; i < kEven; ++i) {
            if (count[static_cast<std::size_t>(i)] > 0.0) {
                carrier_ratio[static_cast<std::size_t>(i)] = std::max(sum[static_cast<std::size_t>(i)] / count[static_cast<std::size_t>(i)] / base, 1.0);
            }
        }
    }
    est.carrier_noise.assign(kCarriers, 1.0f);
    for (int c = 0; c < kCarriers; ++c) {
        // 奇數格沒有導頻，取左右兩個偶數格的平均
        const double ratio = (c % 2 == 0) ? carrier_ratio[static_cast<std::size_t>(c / 2)]
                                          : 0.5 * (carrier_ratio[static_cast<std::size_t>((c - 1) / 2)] + carrier_ratio[static_cast<std::size_t>((c + 1) / 2)]);
        est.carrier_noise[static_cast<std::size_t>(c)] = static_cast<float>(ratio);
    }

    const double mean_h2 = power / static_cast<double>(std::max<long>(data_count, 1));
    const double bias = est.noise_var * gain_sum / static_cast<double>(std::max<long>(data_count, 1));
    est.mean_power = static_cast<float>(std::max(mean_h2 - bias, 1e-3 * mean_h2));
    // 3 kHz 的雜訊頻寬是 64 格（3000 ÷ 46.875）；訊號功率是每個符號所有載波的總和
    est.snr_db = static_cast<float>(10.0 * std::log10(kMeanSymbolPower * est.mean_power / (64.0 * est.noise_var)));
    return est;
}

}  // namespace nbtv
