#include "dsp.hpp"

#include <algorithm>
#include <cmath>

namespace nbtv {

namespace {

// 第一類零階修正貝索函數
double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-16 * sum) break;
    }
    return sum;
}

double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

constexpr int kResampleTaps = 47;       // 以 48 kHz 計
constexpr double kResampleCutoff = 0.125;  // 6 kHz / 48 kHz
constexpr double kResampleBeta = 7.0;

constexpr int kAnalyticTaps = 63;
constexpr double kAnalyticCutoff = 0.125;  // 低通原型 1500 Hz / 12 kHz，再搬到中心 1500 Hz
constexpr double kAnalyticBeta = 3.4;

}  // namespace

std::vector<double> design_lowpass(int taps, double cutoff, double beta) {
    std::vector<double> h(static_cast<std::size_t>(taps));
    const double mid = (taps - 1) / 2.0;
    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        const double r = (n - mid) / mid;
        const double window = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel_i0(beta);
        h[static_cast<std::size_t>(n)] = 2.0 * cutoff * sinc(2.0 * cutoff * (n - mid)) * window;
        sum += h[static_cast<std::size_t>(n)];
    }
    for (double& v : h) v /= sum;
    return h;
}

Decimator4::Decimator4() {
    for (double v : design_lowpass(kResampleTaps, kResampleCutoff, kResampleBeta)) taps_.push_back(static_cast<float>(v));
    history_.assign(taps_.size(), 0.0f);
}

void Decimator4::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    pos_ = 0;
    phase_ = 0;
}

void Decimator4::process(std::span<const float> in, std::vector<float>& out) {
    const std::size_t n = taps_.size();
    for (float x : in) {
        history_[pos_] = x;
        pos_ = (pos_ + 1) % n;
        if (++phase_ == 4) {
            phase_ = 0;
            // history_[pos_] 是最舊的取樣
            float acc = 0.0f;
            std::size_t idx = pos_;
            for (std::size_t t = 0; t < n; ++t) {
                acc += taps_[t] * history_[idx];
                idx = idx + 1 == n ? 0 : idx + 1;
            }
            out.push_back(acc);
        }
    }
}

Interpolator4::Interpolator4() {
    // 補零後濾波等同於四組多相濾波器；增益乘 4 補回補零損失的振幅。
    for (double v : design_lowpass(kResampleTaps, kResampleCutoff, kResampleBeta)) taps_.push_back(static_cast<float>(4.0 * v));
    history_.assign((taps_.size() + 3) / 4, 0.0f);
}

void Interpolator4::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    pos_ = 0;
}

void Interpolator4::process(std::span<const float> in, std::vector<float>& out) {
    const std::size_t hist = history_.size();
    const std::size_t taps = taps_.size();
    for (float x : in) {
        history_[pos_] = x;  // 最新的輸入
        for (std::size_t phase = 0; phase < 4; ++phase) {
            // 輸出第 phase 個相位：taps_[phase + 4m] 對應 m 個輸入之前的取樣
            float acc = 0.0f;
            std::size_t idx = pos_;
            for (std::size_t t = phase; t < taps; t += 4) {
                acc += taps_[t] * history_[idx];
                idx = idx == 0 ? hist - 1 : idx - 1;
            }
            out.push_back(acc);
        }
        pos_ = (pos_ + 1) % hist;
    }
}

AnalyticFilter::AnalyticFilter() {
    const std::vector<double> lp = design_lowpass(kAnalyticTaps, kAnalyticCutoff, kAnalyticBeta);
    const double mid = (kAnalyticTaps - 1) / 2.0;
    const double center = 1500.0 / 12000.0;  // 搬到 1500 Hz：通帶 0–3000 Hz 的正頻率
    for (int n = 0; n < kAnalyticTaps; ++n) {
        const double phase = kTwoPi * center * (n - mid);
        // 乘 2：解析訊號的實部等於原訊號
        taps_.emplace_back(static_cast<float>(2.0 * lp[static_cast<std::size_t>(n)] * std::cos(phase)),
                           static_cast<float>(2.0 * lp[static_cast<std::size_t>(n)] * std::sin(phase)));
    }
    history_.assign(taps_.size(), 0.0f);
}

void AnalyticFilter::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    pos_ = 0;
}

void AnalyticFilter::process(std::span<const float> in, std::vector<cf>& out) {
    const std::size_t n = taps_.size();
    for (float x : in) {
        history_[pos_] = x;
        pos_ = pos_ + 1 == n ? 0 : pos_ + 1;
        // 摺積：y[m] = Σ h[t]·x[m − t]；history_[pos_] 是最舊的（x[m − (n−1)]）
        float re = 0.0f, im = 0.0f;
        std::size_t idx = pos_;
        for (std::size_t t = n; t-- > 0;) {
            re += taps_[t].real() * history_[idx];
            im += taps_[t].imag() * history_[idx];
            idx = idx + 1 == n ? 0 : idx + 1;
        }
        out.emplace_back(re, im);
    }
}

}  // namespace nbtv
