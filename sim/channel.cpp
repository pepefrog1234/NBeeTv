#include "channel.hpp"

#include <algorithm>
#include <cmath>

namespace nbtv::sim {

namespace {

constexpr double kFs = 12000.0;
constexpr int kHilbertTaps = 255;       // 奇數；群延遲 127
constexpr int kFadeInterval = 120;      // 衰落增益每 10 ms 更新一次（100 Hz），中間線性內插
constexpr int kResampleHalf = 12;       // 內插用的 sinc 半寬

double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-16 * sum) break;
    }
    return sum;
}

double kaiser(double r, double beta) { return std::fabs(r) >= 1.0 ? 0.0 : bessel_i0(beta * std::sqrt(1.0 - r * r)) / bessel_i0(beta); }

}  // namespace

bool channel_preset(std::string_view name, ChannelParams& p) {
    if (name == "awgn") {
        p.fading = false;
        p.delay_ms = 0.0;
        p.doppler_hz = 0.0;
    } else if (name == "poor") {
        p.fading = true;
        p.delay_ms = 2.0;
        p.doppler_hz = 1.0;
    } else if (name == "moderate") {
        p.fading = true;
        p.delay_ms = 1.0;
        p.doppler_hz = 0.5;
    } else if (name == "slow") {
        p.fading = true;
        p.delay_ms = 0.5;
        p.doppler_hz = 0.1;
    } else {
        return false;
    }
    return true;
}

cd Channel::Fader::filter() const {
    cd acc(0, 0);
    std::size_t idx = pos;
    for (double t : taps) {
        acc += t * history[idx];
        idx = idx + 1 == history.size() ? 0 : idx + 1;
    }
    return acc;
}

double Channel::interference() {
    double v = 0.0;
    if (params_.tone_hz > 0.0) {
        // 正弦波的功率是振幅平方的一半
        v += std::sqrt(2.0 * signal_power_ * std::pow(10.0, params_.tone_db / 10.0)) * std::sin(tone_phase_);
        tone_phase_ += kTwoPi * params_.tone_hz / kFs;
        if (tone_phase_ > kPi) tone_phase_ -= kTwoPi;
    }
    if (params_.impulse_rate > 0.0) {
        if (impulse_left_ == 0 && rng_.uniform() < params_.impulse_rate / kFs) {
            impulse_left_ = static_cast<int>(params_.impulse_ms * 1e-3 * kFs);
        }
        if (impulse_left_ > 0) {
            --impulse_left_;
            v += std::sqrt(signal_power_ * std::pow(10.0, params_.impulse_db / 10.0)) * rng_.gauss();
        }
    }
    return v;
}

Channel::Channel(const ChannelParams& params, double signal_power) : params_(params), rng_(params.seed), signal_power_(signal_power) {
    // 實數白雜訊在 0–6 kHz 的功率是 σ²，所以 N0 = σ²/6000；訊雜比 = P/(N0·3000) → σ² = 2P/SNR。
    if (params_.add_noise) noise_sigma_ = std::sqrt(2.0 * signal_power / std::pow(10.0, params_.snr_db / 10.0));

    // 希爾伯特轉換器：h[n] = 2/(πn)（n 為奇數），加 Kaiser 窗
    hilbert_.assign(kHilbertTaps, 0.0);
    const int mid = (kHilbertTaps - 1) / 2;
    for (int n = 0; n < kHilbertTaps; ++n) {
        const int k = n - mid;
        if (k % 2 != 0) hilbert_[static_cast<std::size_t>(n)] = 2.0 / (kPi * k) * kaiser(static_cast<double>(k) / (mid + 1), 8.0);
    }
    real_hist_.assign(kHilbertTaps, 0.0);

    delay_samples_ = params_.fading ? static_cast<int>(std::lround(params_.delay_ms * 1e-3 * kFs)) : 0;
    delay_line_.assign(static_cast<std::size_t>(delay_samples_) + 1, cd(0, 0));

    gain_now_[0] = cd(1, 0);
    gain_now_[1] = cd(0, 0);
    fade_interval_ = kFadeInterval;
    if (params_.fading) {
        // 都卜勒功率頻譜 exp(−f²/(2σ²))，σ = 擴散的一半；濾波器是它的平方根，
        // 脈衝響應 exp(−(2πσt)²)。取 ±4.5 個標準差長。
        const double sigma = std::max(params_.doppler_hz, 1e-4) / 2.0;
        const double fade_rate = kFs / kFadeInterval;
        const double sigma_t = 1.0 / (2.0 * std::sqrt(2.0) * kPi * sigma);
        const int half = static_cast<int>(std::ceil(4.5 * sigma_t * fade_rate));
        for (Fader& f : fader_) {
            double energy = 0.0;
            for (int n = -half; n <= half; ++n) {
                const double t = n / fade_rate;
                const double v = std::exp(-(kTwoPi * sigma * t) * (kTwoPi * sigma * t));
                f.taps.push_back(v);
                energy += v * v;
            }
            // 兩條路徑各佔一半功率
            for (double& v : f.taps) v *= std::sqrt(0.5 / energy);
            f.history.resize(f.taps.size());
            for (cd& v : f.history) v = rng_.cgauss();  // 先填滿，開頭就是穩態
            f.current = f.filter();
            f.history[f.pos] = rng_.cgauss();
            f.pos = f.pos + 1 == f.history.size() ? 0 : f.pos + 1;
            f.next = f.filter();
        }
        gain_now_[0] = fader_[0].current;
        gain_now_[1] = fader_[1].current;
    }

    rs_hist_.clear();
    rs_next_ = static_cast<double>(kResampleHalf);
}

void Channel::advance_faders() {
    for (Fader& f : fader_) {
        f.current = f.next;
        f.history[f.pos] = rng_.cgauss();
        f.pos = f.pos + 1 == f.history.size() ? 0 : f.pos + 1;
        f.next = f.filter();
    }
}

void Channel::process(std::span<const float> in, std::vector<float>& out) {
    const std::size_t taps = hilbert_.size();
    const std::size_t mid = (taps - 1) / 2;
    for (float x : in) {
        // 解析訊號：實部是延遲後的輸入，虛部是希爾伯特轉換
        real_hist_[real_pos_] = x;
        real_pos_ = real_pos_ + 1 == taps ? 0 : real_pos_ + 1;
        double im = 0.0;
        std::size_t idx = real_pos_;  // 最舊的
        for (std::size_t t = taps; t-- > 0;) {
            im += hilbert_[t] * real_hist_[idx];
            idx = idx + 1 == taps ? 0 : idx + 1;
        }
        const double re = real_hist_[(real_pos_ + mid) % taps];  // 延遲 mid 個取樣
        cd z(re, im);

        if (params_.fading) {
            const double frac = static_cast<double>(fade_phase_) / fade_interval_;
            gain_now_[0] = fader_[0].current + (fader_[0].next - fader_[0].current) * frac;
            gain_now_[1] = fader_[1].current + (fader_[1].next - fader_[1].current) * frac;
            if (++fade_phase_ == fade_interval_) {
                fade_phase_ = 0;
                advance_faders();
            }
            delay_line_[delay_pos_] = z;
            const std::size_t n = delay_line_.size();
            const cd delayed = delay_line_[(delay_pos_ + 1) % n];  // delay_samples_ 個取樣之前
            delay_pos_ = (delay_pos_ + 1) % n;
            z = gain_now_[0] * z + gain_now_[1] * delayed;
        }

        if (params_.freq_offset_hz != 0.0) {
            z *= std::polar(1.0, offset_phase_);
            offset_phase_ += kTwoPi * params_.freq_offset_hz / kFs;
            if (offset_phase_ > kPi) offset_phase_ -= kTwoPi;
            if (offset_phase_ < -kPi) offset_phase_ += kTwoPi;
        }
        pending_.push_back(z.real());
    }

    if (params_.clock_ppm == 0.0) {
        for (double v : pending_) out.push_back(static_cast<float>(v + noise_sigma_ * rng_.gauss() + interference()));
        pending_.clear();
    } else {
        resample_flush(out);
    }
}

// 接收端時脈偏快 ε：第 m 個輸出取樣對應輸入時間 m/(1+ε)。
void Channel::resample_flush(std::vector<float>& out) {
    rs_hist_.insert(rs_hist_.end(), pending_.begin(), pending_.end());
    pending_.clear();
    const double step = 1.0 / (1.0 + params_.clock_ppm * 1e-6);
    const std::size_t base = rs_count_;  // rs_hist_[0] 的輸入取樣編號
    const double last = static_cast<double>(base + rs_hist_.size());
    while (rs_next_ + kResampleHalf + 1 < last) {
        const double center = rs_next_ - static_cast<double>(base);
        const long first = static_cast<long>(std::floor(center)) - kResampleHalf + 1;
        double acc = 0.0;
        for (int k = 0; k < 2 * kResampleHalf; ++k) {
            const long i = first + k;
            if (i < 0) continue;
            const double d = center - static_cast<double>(i);
            acc += rs_hist_[static_cast<std::size_t>(i)] * sinc(d) * kaiser(d / kResampleHalf, 8.0);
        }
        out.push_back(static_cast<float>(acc + noise_sigma_ * rng_.gauss() + interference()));
        rs_next_ += step;
    }
    // 丟掉不再需要的歷史
    const long keep_from = static_cast<long>(std::floor(rs_next_ - static_cast<double>(base))) - kResampleHalf - 1;
    if (keep_from > 4096) {
        rs_hist_.erase(rs_hist_.begin(), rs_hist_.begin() + keep_from);
        rs_count_ += static_cast<std::size_t>(keep_from);
    }
}

}  // namespace nbtv::sim
