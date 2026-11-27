#include "duplex.hpp"

#include <algorithm>
#include <cmath>

#include "dsp.hpp"
#include "ofdm_tx.hpp"

namespace nbtv {

namespace {

// Kaiser 窗的低通要幾個係數、窗的參數是多少：過渡帶寬 width（以取樣率為 1）、阻帶壓 attenuation_db。
int kaiser_length(double width, double attenuation_db) {
    return static_cast<int>(std::ceil((attenuation_db - 7.95) / (2.285 * kTwoPi * width))) + 1;
}

double kaiser_beta(double attenuation_db) {
    if (attenuation_db > 50.0) return 0.1102 * (attenuation_db - 8.7);
    if (attenuation_db > 21.0) return 0.5842 * std::pow(attenuation_db - 21.0, 0.4) + 0.07886 * (attenuation_db - 21.0);
    return 0.0;
}

constexpr double kResampleAttenuationDb = 70.0;  // 重取樣時疊進來的、多出來的鏡像壓這麼多
constexpr double kMoverAttenuationDb = 60.0;     // 另一路的訊號壓這麼多

// ── 畫面 ──
// 數據機的頻帶以 1500 Hz 為中心（12 kHz 取樣時每個取樣 0.125 圈），寬 ±1148 Hz。
constexpr double kPictureHalfBand = 0.5 * (kModemHigh - kModemLow);
constexpr double kPictureCentre = 0.5 * (kModemHigh + kModemLow) / kDuplexPictureRate;
// 濾波器的通帶比數據機佔的範圍每邊多留這麼多 Hz（12 kHz 取樣時）：最外面的載波離範圍的邊緣只有半格
constexpr double kPictureMargin = 25.0;

// 放慢之後、移好位置的畫面，中心在每個取樣幾圈的地方（取樣還是數據機的那些，只是每秒播 12000/k 個）
double picture_air_centre(DuplexLayout layout) {
    return (kPictureLow * duplex_slow_factor(layout) + kPictureHalfBand) / kDuplexPictureRate;
}

double picture_pass() { return (kPictureHalfBand + kPictureMargin) / kDuplexPictureRate; }

// 阻帶從語音的上緣開始：它離畫面的下緣 115 Hz，換成數據機的取樣是 k 倍
double picture_stop(DuplexLayout layout) {
    const double gap = kPictureLow - (kVoiceCentre + kVoiceHalfBand);
    return (kPictureHalfBand + gap * duplex_slow_factor(layout)) / kDuplexPictureRate;
}

// 放慢之後的取樣率，和它與 48 kHz 之間的比例（48000 = 取樣率 × up / down）。疊加的版面不放慢：12 kHz 直接變 4 倍。
double picture_air_rate(DuplexLayout layout) { return kDuplexPictureRate / duplex_slow_factor(layout); }
int picture_up(DuplexLayout layout) { return layout == DuplexLayout::Overlay ? 4 : layout == DuplexLayout::Narrow ? 36 : 6; }
int picture_down(DuplexLayout layout) { return layout == DuplexLayout::Narrow ? 5 : 1; }

// 重取樣要留住的範圍（Hz）到畫面的上緣；阻帶從會疊回這個範圍的地方開始
double picture_resample_pass(DuplexLayout layout) { return duplex_picture_high(layout) + kPictureMargin; }
double picture_resample_stop(DuplexLayout layout) { return picture_air_rate(layout) - picture_resample_pass(layout); }

// ── 語音 ──
constexpr double kVoiceResamplePass = kVoiceCentre + kVoiceStopBand;
constexpr int kVoiceRatio = kDuplexRate / kDuplexVoiceRate;
// 疊加的版面：語音不移頻，8 kHz 與 48 kHz 之間留到 3 kHz（RADE V1 削波之後帶外還有一點裙邊，SSB 的濾波器之內的都留著）
constexpr double kOverlayResamplePass = 3000.0;
double voice_resample_pass(DuplexLayout layout) { return duplex_overlay(layout) ? kOverlayResamplePass : kVoiceResamplePass; }
double voice_resample_stop(DuplexLayout layout) { return kDuplexVoiceRate - voice_resample_pass(layout); }
// 搬頻的濾波器：分頻的版面把語音在 1500 Hz 與 790 Hz 之間搬來搬去；疊加的版面不搬（from 與 to 相同），
// 只在接收端用它把實數的音訊變成解析訊號。
double voice_air_centre(DuplexLayout layout) { return (duplex_overlay(layout) ? kOverlayVoiceCentre : kVoiceCentre) / kDuplexVoiceRate; }
double voice_codec_centre(DuplexLayout layout) { return (duplex_overlay(layout) ? kOverlayVoiceCentre : kVoiceInputCentre) / kDuplexVoiceRate; }
double voice_pass(DuplexLayout layout) { return (duplex_overlay(layout) ? kOverlayVoiceHalfBand : kVoiceHalfBand) / kDuplexVoiceRate; }
double voice_stop(DuplexLayout layout) { return (duplex_overlay(layout) ? kOverlayVoiceStopBand : kVoiceStopBand) / kDuplexVoiceRate; }

// ── 兩路相加的比例 ──
// 畫面交進來時的均方根：數據機把包絡的平均功率定成 1、削波門檻對應到 1/kTxHeadroom，實數訊號的功率是包絡的一半。
double picture_nominal_rms() { return 1.0 / (std::pow(10.0, kDefaultPaprDb / 20.0) * kTxHeadroom * std::sqrt(2.0)); }
// 兩路各自過了這裡的濾波器之後最大的峰值（交進來的刻度；實測的最大值再留一點）。相加之後最壞就是兩個峰值碰在一起。
constexpr double kPicturePeak = 1.0;
constexpr double kVoicePeak = 1.1;
// 疊加的版面：RADE V1 的訊號在 8 kHz 的取樣點上大小不超過 1，變成 48 kHz 之後取樣點之間會高一點
// （50 秒的語音實測最大 1.27，萬分之一的取樣超過 1.15）
constexpr double kOverlayVoicePeak = 1.3;
double voice_peak(DuplexLayout layout) { return duplex_overlay(layout) ? kOverlayVoicePeak : kVoicePeak; }
constexpr float kVoiceGainMinDb = -20.0f, kVoiceGainMaxDb = 12.0f;

// 等著被取走的取樣：前面已經取走的夠多了就丟掉，免得愈積愈長
template <typename T>
void drop_consumed(std::vector<T>& buffer, std::size_t& read) {
    if (read >= 16384 && read * 2 >= buffer.size()) {
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
        read = 0;
    }
}

}  // namespace

namespace duplex_detail {

Resampler::Resampler(int up, int down, double input_rate, double pass_hz, double stop_hz) : up_(up), down_(down) {
    // 原型濾波器在「輸入補零成 up 倍」的取樣率上；長度取 up 的倍數，每一相一樣多
    const double rate = input_rate * up;
    const double pass = pass_hz / rate, stop = stop_hz / rate;
    const int wanted = kaiser_length(stop - pass, kResampleAttenuationDb);
    per_phase_ = static_cast<std::size_t>((wanted + up - 1) / up);
    const std::vector<double> h = design_lowpass(static_cast<int>(per_phase_) * up, 0.5 * (pass + stop), kaiser_beta(kResampleAttenuationDb));
    taps_.resize(h.size());
    for (int p = 0; p < up; ++p) {
        for (std::size_t i = 0; i < per_phase_; ++i) {
            // 第 p 相用原型的第 p、p + up、p + 2·up… 個係數，依序乘上最新、次新…的輸入；這裡倒過來存（最舊的在前）。
            // 乘 up 補回補零損失的振幅。
            const std::size_t j = per_phase_ - 1 - i;
            taps_[static_cast<std::size_t>(p) * per_phase_ + i] = static_cast<float>(up * h[static_cast<std::size_t>(p) + j * static_cast<std::size_t>(up)]);
        }
    }
    history_.assign(2 * per_phase_, 0.0f);
}

void Resampler::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    pos_ = 0;
    phase_ = 0;
}

void Resampler::process(std::span<const float> in, std::vector<float>& out) {
    const std::size_t n = per_phase_;
    for (float x : in) {
        history_[pos_] = history_[pos_ + n] = x;
        pos_ = pos_ + 1 == n ? 0 : pos_ + 1;
        const float* window = history_.data() + pos_;  // 最近的 n 個輸入，最舊的在前
        // phase_：下一個輸出落在「補零之後」的第幾個位置（相對於最新的這個輸入）。小於 up 的都可以算了。
        while (phase_ < up_) {
            const float* taps = taps_.data() + static_cast<std::size_t>(phase_) * n;
            float acc = 0.0f;
            for (std::size_t i = 0; i < n; ++i) acc += taps[i] * window[i];
            out.push_back(acc);
            phase_ += down_;
        }
        phase_ -= up_;
    }
}

BandMover::BandMover(double from, double to, double pass, double stop, double attenuation_db) : from_(from), to_(to) {
    const int taps = kaiser_length(stop - pass, attenuation_db) | 1;  // 奇數個：延遲是整數個取樣
    for (double v : design_lowpass(taps, 0.5 * (pass + stop), kaiser_beta(attenuation_db))) taps_.push_back(static_cast<float>(v));
    history_.assign(2 * taps_.size(), cf(0, 0));
}

void BandMover::reset() {
    std::fill(history_.begin(), history_.end(), cf(0, 0));
    pos_ = 0;
    phase_in_ = phase_out_ = 0.0;
}

cf BandMover::step(cf mixed) {
    const std::size_t n = taps_.size();
    history_[pos_] = history_[pos_ + n] = mixed;
    pos_ = pos_ + 1 == n ? 0 : pos_ + 1;
    const cf* window = history_.data() + pos_;
    float re = 0.0f, im = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        re += taps_[i] * window[i].real();
        im += taps_[i] * window[i].imag();
    }
    const double angle = kTwoPi * phase_out_;
    phase_out_ += to_;
    phase_out_ -= std::floor(phase_out_);
    return cf(re, im) * cf(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
}

void BandMover::process(std::span<const float> in, std::vector<cf>& out) {
    for (float x : in) {
        const double angle = -kTwoPi * phase_in_;
        phase_in_ += from_;
        phase_in_ -= std::floor(phase_in_);
        out.push_back(step(cf(x * static_cast<float>(std::cos(angle)), x * static_cast<float>(std::sin(angle)))));
    }
}

void BandMover::process(std::span<const cf> in, std::vector<cf>& out) {
    for (cf x : in) {
        const double angle = -kTwoPi * phase_in_;
        phase_in_ += from_;
        phase_in_ -= std::floor(phase_in_);
        out.push_back(step(x * cf(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)))));
    }
}

}  // namespace duplex_detail

DuplexTx::DuplexTx(DuplexLayout layout)
    : layout_(layout),
      picture_mover_(kPictureCentre, picture_air_centre(layout), picture_pass(), picture_stop(layout), kMoverAttenuationDb),
      picture_up_(picture_up(layout), picture_down(layout), picture_air_rate(layout), picture_resample_pass(layout), picture_resample_stop(layout)),
      voice_mover_(voice_codec_centre(layout), voice_air_centre(layout), voice_pass(layout), voice_stop(layout), kMoverAttenuationDb),
      voice_up_(kVoiceRatio, 1, kDuplexVoiceRate, voice_resample_pass(layout), voice_resample_stop(layout)) {
    set_voice_gain_db(0.0f);
}

void DuplexTx::set_voice_gain_db(float db) {
    // 兩路的功率比照設定的來，整體的大小由「兩個峰值碰在一起也不超過滿刻度」決定。
    const double ratio = std::pow(10.0, std::clamp(std::isfinite(db) ? db : 0.0f, kVoiceGainMinDb, kVoiceGainMaxDb) / 20.0);
    const double picture_rms = picture_nominal_rms();
    const double voice_rms = duplex_voice_nominal_rms(layout_) / std::sqrt(2.0);  // 送出去的是實部
    const double voice_per_picture = ratio * picture_rms / voice_rms;
    const double picture_gain = 1.0 / (kPicturePeak + voice_peak(layout_) * voice_per_picture);
    picture_gain_ = static_cast<float>(picture_gain);
    voice_gain_ = static_cast<float>(picture_gain * voice_per_picture);
}

void DuplexTx::reset() {
    picture_mover_.reset();
    picture_up_.reset();
    voice_mover_.reset();
    voice_up_.reset();
    picture_.clear();
    voice_.clear();
    picture_read_ = voice_read_ = 0;
}

void DuplexTx::push_picture(std::span<const float> samples) {
    drop_consumed(picture_, picture_read_);
    if (duplex_overlay(layout_)) {
        // 疊加：畫面原樣，只換成 48 kHz
        picture_up_.process(samples, picture_);
        return;
    }
    scratch_.clear();
    picture_mover_.process(samples, scratch_);
    real_.resize(scratch_.size());
    // 實數訊號只有一半在正頻率：搬過去之後乘 2 補回來
    for (std::size_t i = 0; i < scratch_.size(); ++i) real_[i] = 2.0f * scratch_[i].real();
    picture_up_.process(real_, picture_);
}

void DuplexTx::flush_picture() {
    // 搬頻的濾波器延遲最長（一百多個取樣），重取樣的只有十個左右：多給一點（疊加的版面沒有搬頻那一段）
    const std::vector<float> quiet(static_cast<std::size_t>(duplex_overlay(layout_) ? 0 : picture_mover_.delay_samples()) + 32, 0.0f);
    push_picture(quiet);
}

void DuplexTx::push_voice(std::span<const cf> samples) {
    drop_consumed(voice_, voice_read_);
    if (duplex_overlay(layout_)) {
        // 疊加：語音的訊號原樣（不濾、不移），取實部換成 48 kHz
        real_.resize(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) real_[i] = samples[i].real();
        voice_up_.process(real_, voice_);
        return;
    }
    scratch_.clear();
    voice_mover_.process(samples, scratch_);
    real_.resize(scratch_.size());
    for (std::size_t i = 0; i < scratch_.size(); ++i) real_[i] = scratch_[i].real();
    voice_up_.process(real_, voice_);
}

void DuplexTx::read(std::span<float> out) {
    for (float& sample : out) {
        const float picture = picture_read_ < picture_.size() ? picture_[picture_read_++] : 0.0f;
        const float voice = voice_read_ < voice_.size() ? voice_[voice_read_++] : 0.0f;
        sample = std::clamp(picture_gain_ * picture + voice_gain_ * voice, -1.0f, 1.0f);
    }
}

DuplexRx::DuplexRx(DuplexLayout layout)
    : layout_(layout),
      picture_down_(picture_down(layout), picture_up(layout), kDuplexRate, picture_resample_pass(layout), picture_resample_stop(layout)),
      picture_mover_(picture_air_centre(layout), kPictureCentre, picture_pass(), picture_stop(layout), kMoverAttenuationDb),
      voice_down_(1, kVoiceRatio, kDuplexRate, voice_resample_pass(layout), voice_resample_stop(layout)),
      voice_mover_(voice_air_centre(layout), voice_codec_centre(layout), voice_pass(layout), voice_stop(layout), kMoverAttenuationDb) {}

void DuplexRx::reset() {
    picture_down_.reset();
    picture_mover_.reset();
    voice_down_.reset();
    voice_mover_.reset();
    picture_.clear();
    voice_.clear();
    picture_read_ = voice_read_ = 0;
}

void DuplexRx::push(std::span<const float> samples) {
    drop_consumed(picture_, picture_read_);
    drop_consumed(voice_, voice_read_);
    clean_samples(samples, clean_);
    if (want_picture_ && duplex_overlay(layout_)) {
        // 疊加：畫面沒有放慢、沒有搬過，只換回 12 kHz（語音還疊在上面，接收機把它當成雜訊）
        picture_down_.process(clean_, picture_);
    } else if (want_picture_) {
        // 畫面：換成放慢之後的取樣率（那些取樣就是數據機的取樣），搬回數據機原來的位置
        real_.clear();
        picture_down_.process(clean_, real_);
        scratch_.clear();
        picture_mover_.process(real_, scratch_);
        for (cf v : scratch_) picture_.push_back(2.0f * v.real());
    }
    if (want_voice_) {
        // 語音：換成 8 kHz，搬回中心 1500 Hz（疊加的版面不搬，這一步只是把實數的音訊變成解析訊號）
        real_.clear();
        voice_down_.process(clean_, real_);
        scratch_.clear();
        voice_mover_.process(real_, scratch_);
        for (cf v : scratch_) voice_.push_back(2.0f * v);
    }
}

std::size_t DuplexRx::read_picture(std::span<float> out) {
    const std::size_t count = std::min(out.size(), available_picture());
    std::copy_n(picture_.begin() + static_cast<std::ptrdiff_t>(picture_read_), count, out.begin());
    picture_read_ += count;
    return count;
}

std::size_t DuplexRx::read_voice(std::span<cf> out) {
    const std::size_t count = std::min(out.size(), available_voice());
    std::copy_n(voice_.begin() + static_cast<std::ptrdiff_t>(voice_read_), count, out.begin());
    voice_read_ += count;
    return count;
}

}  // namespace nbtv
