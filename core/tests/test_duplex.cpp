// 語音與畫面一起送（duplex.hpp）：重取樣、搬頻、兩路的合成與分離，
// 以及畫面經過這一層之後原來的接收機照樣收得到（包括兩邊音效卡的取樣時脈有差的時候）。
// 三種版面：FM 語音通道分頻的兩種，和語音直接疊在畫面上的「疊加」。
// 核心不含語音的編解碼器，這裡的「語音」是替身：和 RADE V2（或 V1）同樣位置、同樣寬的一組單音。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "check.hpp"
#include "duplex.hpp"
#include "link_util.hpp"
#include "rng.hpp"
#include "test_images.hpp"

using namespace nbtv;
using namespace link_util;
using duplex_detail::BandMover;
using duplex_detail::Resampler;

namespace {

std::vector<float> make_tone(double hz, double rate, std::size_t count) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) x[n] = static_cast<float>(std::cos(kTwoPi * hz * static_cast<double>(n) / rate));
    return x;
}

// x 的 [from, to) 這一段裡，頻率 hz 的成分的振幅。加 Hann 窗：別的頻率再強也漏不進來。
double tone_amplitude(std::span<const float> x, double hz, double rate, std::size_t from, std::size_t to) {
    cd acc(0, 0);
    double weight = 0.0;
    const double length = static_cast<double>(to - from);
    for (std::size_t n = from; n < to; ++n) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * (static_cast<double>(n - from) + 0.5) / length);
        acc += w * static_cast<double>(x[n]) * std::polar(1.0, -kTwoPi * hz * static_cast<double>(n) / rate);
        weight += w;
    }
    return 2.0 * std::abs(acc) / weight;
}

double rms(std::span<const float> x) { return std::sqrt(mean_power(x)); }

// 重取樣：通帶裡振幅不變，會疊進來的、多出來的鏡像壓 60 dB 以上；分幾次給和一次給完全相同
void test_resampler() {
    struct Case {
        int up, down;
        double in_rate, pass, stop;
    };
    // 兩種分頻的版面畫面那一路的往上、往下，以及語音那一路的；最後四個是疊加的版面的（畫面 12 kHz ↔ 48 kHz，語音留到 3 kHz）
    for (const Case& c : {Case{6, 1, 8000.0, 2891.0, 5109.0}, Case{1, 6, 48000.0, 2891.0, 5109.0}, Case{36, 5, 12000.0 / 1.8, 2636.0, 4031.0},
                          Case{5, 36, 48000.0, 2636.0, 4031.0}, Case{6, 1, 8000.0, 1335.0, 6665.0}, Case{1, 6, 48000.0, 1335.0, 6665.0},
                          Case{4, 1, 12000.0, 2673.0, 9327.0}, Case{1, 4, 48000.0, 2673.0, 9327.0}, Case{6, 1, 8000.0, 3000.0, 5000.0},
                          Case{1, 6, 48000.0, 3000.0, 5000.0}}) {
        const double out_rate = c.in_rate * c.up / c.down;
        const std::size_t count = static_cast<std::size_t>(c.in_rate);  // 一秒
        double worst_pass = 0.0, worst_stop = 0.0;
        for (double hz : {300.0, 0.5 * c.pass, c.pass}) {
            Resampler r(c.up, c.down, c.in_rate, c.pass, c.stop);
            std::vector<float> out;
            r.process(make_tone(hz, c.in_rate, count), out);
            const long long expected = std::llround(static_cast<double>(count) * c.up / c.down);
            CHECK(std::llabs(static_cast<long long>(out.size()) - expected) <= 1);
            const std::size_t skip = out.size() / 4;
            const double amplitude = tone_amplitude(out, hz, out_rate, skip, out.size());
            CHECK_NEAR(amplitude, 1.0, 2e-3);
            worst_pass = std::max(worst_pass, std::fabs(amplitude - 1.0));
            if (c.up > c.down) {  // 往上：補零產生的鏡像（輸入的取樣率減掉這個頻率）
                const double image = tone_amplitude(out, c.in_rate - hz, out_rate, skip, out.size());
                CHECK(image < 1e-3);
                worst_stop = std::max(worst_stop, image);
            }
        }
        if (c.up < c.down) {  // 往下：阻帶裡的單音會疊到輸出的取樣率減掉它的地方
            for (double hz : {c.stop, c.stop + 700.0, 0.5 * c.in_rate - 1000.0}) {
                Resampler r(c.up, c.down, c.in_rate, c.pass, c.stop);
                std::vector<float> out;
                r.process(make_tone(hz, c.in_rate, count), out);
                const double folded = std::fabs(hz - out_rate * std::round(hz / out_rate));
                const double alias = tone_amplitude(out, folded, out_rate, out.size() / 4, out.size());
                CHECK(alias < 1e-3);
                worst_stop = std::max(worst_stop, alias);
            }
        }
        std::printf("  重取樣 ×%d/%d（%.0f Hz 進）：通帶的誤差最多 %.1e，阻帶最多漏 %.1f dB\n", c.up, c.down, c.in_rate, worst_pass,
                    20.0 * std::log10(std::max(worst_stop, 1e-9)));

        // 分幾次給
        Rng rng(11);
        std::vector<float> noise(5000);
        for (float& v : noise) v = static_cast<float>(rng.gauss());
        Resampler whole(c.up, c.down, c.in_rate, c.pass, c.stop), pieces(c.up, c.down, c.in_rate, c.pass, c.stop);
        std::vector<float> a, b;
        whole.process(noise, a);
        for (std::size_t pos = 0; pos < noise.size();) {
            const std::size_t n = std::min<std::size_t>(1 + rng.below(97), noise.size() - pos);
            pieces.process(std::span<const float>(noise).subspan(pos, n), b);
            pos += n;
        }
        CHECK(a == b);
        // reset 之後和新的一樣
        whole.reset();
        std::vector<float> again;
        whole.process(noise, again);
        CHECK(a == again);
    }
}

// 搬頻：通帶裡的成分原樣搬到新的位置，阻帶裡的壓 60 dB
void test_band_mover() {
    const double from = 0.125, to = 0.2625, pass = 1173.0 / 12000.0, stop = 1320.5 / 12000.0;
    const std::size_t count = 12000;
    auto moved_amplitude = [&](double offset, bool real_input) {
        BandMover mover(from, to, pass, stop, 60.0);
        std::vector<cf> out;
        if (real_input) {
            mover.process(std::span<const float>(make_tone((from + offset) * 12000.0, 12000.0, count)), out);
        } else {
            std::vector<cf> in(count);
            for (std::size_t n = 0; n < count; ++n) in[n] = cf(std::polar(1.0, kTwoPi * (from + offset) * static_cast<double>(n)));
            mover.process(std::span<const cf>(in), out);
        }
        CHECK_EQ(static_cast<long long>(out.size()), static_cast<long long>(count));
        // 輸出裡 to + offset 那個頻率的成分
        cd acc(0, 0);
        const std::size_t skip = count / 4;
        for (std::size_t n = skip; n < count; ++n) acc += cd(out[n]) * std::polar(1.0, -kTwoPi * (to + offset) * static_cast<double>(n));
        return std::abs(acc) / static_cast<double>(count - skip);
    };
    for (double offset : {-pass, -0.03, 0.0, 0.05, pass}) {
        CHECK_NEAR(moved_amplitude(offset, false), 1.0, 2e-3);
        CHECK_NEAR(moved_amplitude(offset, true), 0.5, 1e-3);  // 實數的單音只有一半在正頻率
    }
    double worst = 0.0;
    for (double offset : {-stop, stop, -0.2, 0.3, 0.45}) worst = std::max(worst, moved_amplitude(offset, false));
    CHECK(worst < 1.5e-3);
    std::printf("  搬頻：阻帶最多漏 %.1f dB\n", 20.0 * std::log10(std::max(worst, 1e-9)));
    BandMover mover(from, to, pass, stop, 60.0);
    CHECK(mover.delay_samples() > 100 && mover.delay_samples() < 200);
}

void test_numbers() {
    CHECK_NEAR(duplex_slow_factor(DuplexLayout::Standard), 1.5, 1e-12);
    CHECK_NEAR(duplex_slow_factor(DuplexLayout::Narrow), 1.8, 1e-12);
    CHECK_NEAR(duplex_picture_high(DuplexLayout::Standard), 2865.67, 0.01);
    CHECK_NEAR(duplex_picture_high(DuplexLayout::Narrow), 2610.56, 0.01);
    CHECK_NEAR(duplex_band_shift_hz(DuplexLayout::Standard), 1650.5, 1e-9);
    CHECK_NEAR(duplex_band_shift_hz(DuplexLayout::Narrow), 2051.0, 1e-9);
    // 語音的上緣到畫面的下緣之間留了一段空隙
    CHECK(kVoiceCentre + kVoiceHalfBand < kPictureLow && kVoiceCentre + kVoiceStopBand <= kPictureLow);
    CHECK_NEAR(duplex_voice_low(DuplexLayout::Standard), 360.0, 1e-9);
    CHECK_NEAR(duplex_voice_high(DuplexLayout::Narrow), 1220.0, 1e-9);
    CHECK_NEAR(duplex_picture_low(DuplexLayout::Standard), kPictureLow, 0.0);
    CHECK_EQ(duplex_voice_codec(DuplexLayout::Standard), 2);
    CHECK_EQ(duplex_voice_codec(DuplexLayout::Narrow), 2);
    // 疊加：畫面沒有放慢也沒有搬；語音是 RADE V1 的，整個落在畫面的頻帶裡
    const DuplexLayout overlay = DuplexLayout::Overlay;
    CHECK_NEAR(duplex_slow_factor(overlay), 1.0, 0.0);
    CHECK_NEAR(duplex_picture_low(overlay), kModemLow, 0.0);
    CHECK_NEAR(duplex_picture_high(overlay), kModemHigh, 0.0);
    CHECK_NEAR(duplex_band_shift_hz(overlay), 0.0, 0.0);
    CHECK_EQ(duplex_voice_codec(overlay), 1);
    CHECK(duplex_voice_low(overlay) > duplex_picture_low(overlay) && duplex_voice_high(overlay) < duplex_picture_high(overlay));
    CHECK_NEAR(0.5 * (duplex_voice_low(overlay) + duplex_voice_high(overlay)), kOverlayVoiceCentre, 1e-9);
}

// 語音的替身：8 kHz 的複數，一組等距的單音，均方根是那一版 RADE 平常的大小。各個載波的相位取成包絡起伏小的那一種。
//   V2  中心 1500 Hz，14 個載波相隔 62.5 Hz。128 個取樣重複一次
//   V1  中心 1477.5 Hz，30 個載波相隔 50 Hz。3200 個取樣重複一次
struct VoiceTones {
    double centre;
    int tones;
    double spacing;
    double rms;
    std::size_t period;  // 幾個取樣重複一次：看這麼長（或它的整數倍）時各個單音彼此正交
    double hz(int k) const { return centre + (k - 0.5 * (tones - 1)) * spacing; }
};
constexpr VoiceTones kLikeV2{kVoiceInputCentre, 14, 62.5, kVoiceNominalRms, 128};
constexpr VoiceTones kLikeV1{kOverlayVoiceCentre, 30, 50.0, kOverlayVoiceNominalRms, 3200};

std::vector<cf> stand_in_voice(std::size_t count, const VoiceTones& like = kLikeV2) {
    std::vector<cf> v(count);
    for (std::size_t n = 0; n < count; ++n) {
        cd acc(0, 0);
        for (int k = 0; k < like.tones; ++k) {
            acc += std::polar(1.0, kTwoPi * like.hz(k) * static_cast<double>(n) / kDuplexVoiceRate + kPi * k * k / like.tones);
        }
        v[n] = cf(acc * (like.rms / std::sqrt(static_cast<double>(like.tones))));
    }
    return v;
}

// 像雜訊的替身，給「疊加」用：畫面的接收機眼裡，RADE V1 的訊號就是 728–2228 Hz 裡的一層雜訊。和它一樣 30 個載波、
// 每個符號（含循環字首 24 ms）各載波換一次隨機的相位，大小削到 1 以內（它的訊號也是削成幾乎等幅的），均方根約 0.9。
std::vector<cf> noise_like_voice(std::size_t count, std::uint64_t seed) {
    constexpr int kBody = 160, kPrefix = 32;
    Rng rng(seed);
    std::vector<cf> v;
    v.reserve(count + kBody + kPrefix);
    while (v.size() < count) {
        std::array<double, 30> phase{};
        for (double& p : phase) p = kTwoPi * rng.uniform();
        std::array<cd, kBody> body{};
        for (int n = 0; n < kBody; ++n) {
            cd acc(0, 0);
            for (int k = 0; k < kLikeV1.tones; ++k) acc += std::polar(1.0, kTwoPi * kLikeV1.hz(k) * n / kDuplexVoiceRate + phase[static_cast<std::size_t>(k)]);
            acc *= 1.6 / std::sqrt(static_cast<double>(kLikeV1.tones));  // 均方根 1.6：削到 1 之後剩 0.9 左右
            body[static_cast<std::size_t>(n)] = std::abs(acc) > 1.0 ? acc / std::abs(acc) : acc;
        }
        for (int n = kBody - kPrefix; n < kBody; ++n) v.push_back(cf(body[static_cast<std::size_t>(n)]));
        for (int n = 0; n < kBody; ++n) v.push_back(cf(body[static_cast<std::size_t>(n)]));
    }
    v.resize(count);
    return v;
}

struct VoiceQuality {
    double gain_db = -200.0;   // 收到的那些單音合起來，比送出的大幾 dB
    double snr_db = -200.0;    // 那些單音比其餘的東西大幾 dB
    double ripple_db = 200.0;  // 最強與最弱的單音差幾 dB
};

// 收到的語音訊號裡，替身的那些單音各有多大。看 [from, from + length)，length 取 like.period 的倍數（各個單音彼此正交）。
VoiceQuality voice_quality(std::span<const cf> got, std::size_t from, std::size_t length, const VoiceTones& like = kLikeV2) {
    VoiceQuality q;
    if (from + length > got.size() || length == 0) return q;
    double total = 0.0, tones = 0.0, strongest = 0.0, weakest = 1e30;
    for (std::size_t n = from; n < from + length; ++n) total += std::norm(got[n]);
    total /= static_cast<double>(length);
    for (int k = 0; k < like.tones; ++k) {
        cd acc(0, 0);
        for (std::size_t n = from; n < from + length; ++n) {
            acc += cd(got[n]) * std::polar(1.0, -kTwoPi * like.hz(k) * static_cast<double>(n) / kDuplexVoiceRate);
        }
        const double power = std::norm(acc / static_cast<double>(length));
        tones += power;
        strongest = std::max(strongest, power);
        weakest = std::min(weakest, power);
    }
    q.gain_db = 10.0 * std::log10(std::max(tones, 1e-20) / (like.rms * like.rms));
    q.snr_db = 10.0 * std::log10(std::max(tones, 1e-20) / std::max(total - tones, 1e-20));
    q.ripple_db = 10.0 * std::log10(std::max(strongest, 1e-20) / std::max(weakest, 1e-20));
    return q;
}

// 畫面（12 kHz）與語音（8 kHz）合成送給電台的音訊（48 kHz）。任一路可以是空的。
std::vector<float> combine(DuplexLayout layout, std::span<const float> picture, std::span<const cf> voice, float voice_gain_db = 0.0f) {
    DuplexTx tx(layout);
    tx.set_voice_gain_db(voice_gain_db);
    tx.push_picture(picture);
    tx.push_voice(voice);
    // 後面補一小段靜音，把濾波器裡的尾巴推出來
    const std::vector<float> quiet(1200, 0.0f);
    const std::vector<cf> quiet_voice(800, cf(0, 0));
    if (!picture.empty()) tx.push_picture(quiet);
    if (!voice.empty()) tx.push_voice(quiet_voice);
    std::vector<float> out(std::max(tx.pending_picture(), tx.pending_voice()));
    tx.read(out);
    CHECK_EQ(static_cast<long long>(tx.pending_picture()), 0);
    CHECK_EQ(static_cast<long long>(tx.pending_voice()), 0);
    return out;
}

struct Separated {
    std::vector<float> picture;
    std::vector<cf> voice;
};

Separated separate(DuplexLayout layout, std::span<const float> audio, std::size_t chunk = 4800) {
    DuplexRx rx(layout);
    Separated out;
    std::vector<float> block(1000);
    std::vector<cf> voice_block(1000);
    for (std::size_t pos = 0; pos < audio.size(); pos += chunk) {
        rx.push(audio.subspan(pos, std::min(chunk, audio.size() - pos)));
        for (std::size_t n; (n = rx.read_picture(block)) > 0;) out.picture.insert(out.picture.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(n));
        for (std::size_t n; (n = rx.read_voice(voice_block)) > 0;) out.voice.insert(out.voice.end(), voice_block.begin(), voice_block.begin() + static_cast<std::ptrdiff_t>(n));
    }
    CHECK_EQ(static_cast<long long>(rx.available_picture()), 0);
    CHECK_EQ(static_cast<long long>(rx.available_voice()), 0);
    return out;
}

// 一次發射（畫面輪流用三張測試圖），前後補靜音；12 kHz
std::vector<float> picture_signal(const Mode& mode, int frames) {
    std::vector<float> x(3000, 0.0f);
    const std::vector<float> frames_audio = burst(mode, frames, "N0CALL");
    x.insert(x.end(), frames_audio.begin(), frames_audio.end());
    x.insert(x.end(), 6000, 0.0f);
    return x;
}

struct Reception {
    int good = 0;       // 有畫面、標頭也對的幀數
    int pictures = 0;   // 有畫面的幀數（標頭對不對都算）
    int reported = 0;   // 接收機交出來的幀數
    double psnr = 0.0;  // good 的那些對原圖的平均
    double snr = 0.0;   // 接收機量到的訊雜比的平均
};

Reception receive(const Mode& mode, std::span<const float> audio, double band_shift_hz, bool quiet_carriers = true) {
    Receiver rx;
    attach_models(rx);
    rx.tuning().band_shift_hz = band_shift_hz;
    rx.tuning().quiet_carriers = quiet_carriers;
    Reception r;
    for (const RxFrame& f : run_receiver(rx, audio)) {
        ++r.reported;
        r.pictures += (f.decoded && f.mode_id == mode.id) ? 1 : 0;
        if (!(f.decoded && f.header_ok && f.callsign == "N0CALL" && f.mode_id == mode.id)) continue;
        ++r.good;
        r.psnr += test_images::psnr(f.rgb, test_images::load(test_images::kNames[f.sequence % 3], mode.image_width, mode.image_height));
        r.snr += f.snr_db;
    }
    if (r.good > 0) {
        r.psnr /= r.good;
        r.snr /= r.good;
    }
    return r;
}

// 畫面和語音一起送：每一幀都收到，畫質和不經過這一層直接收的幾乎一樣；語音也原樣回來
void test_together(DuplexLayout layout, int mode_id, int frames) {
    const Mode& mode = *find_mode(mode_id);
    const std::vector<float> picture = picture_signal(mode, frames);
    const double seconds = static_cast<double>(picture.size()) / kDuplexPictureRate * duplex_slow_factor(layout);
    const std::vector<cf> voice = stand_in_voice(static_cast<std::size_t>(seconds * kDuplexVoiceRate));
    const Reception direct = receive(mode, picture, 0.0);
    CHECK_EQ(direct.good, frames);

    const std::vector<float> air = combine(layout, picture, voice);
    // 在空中的時間是原來的 k 倍
    CHECK_NEAR(static_cast<double>(air.size()) / kDuplexRate, seconds, 0.2);
    float peak = 0.0f;
    long clipped = 0;
    for (float v : air) {
        peak = std::max(peak, std::fabs(v));
        clipped += std::fabs(v) >= 1.0f ? 1 : 0;
    }
    CHECK(peak <= 1.0f && peak > 0.5f);
    CHECK_EQ(clipped, 0);

    const Separated got = separate(layout, air);
    CHECK_NEAR(static_cast<double>(got.picture.size()), static_cast<double>(picture.size()), 1500.0);
    const Reception through = receive(mode, got.picture, duplex_band_shift_hz(layout));
    CHECK_EQ(through.good, frames);
    CHECK_EQ(through.reported, frames);
    CHECK(through.psnr > direct.psnr - 0.3);
    CHECK(through.snr > 18.0);

    const VoiceQuality q = voice_quality(got.voice, got.voice.size() / 4, 128 * (got.voice.size() / 2 / 128));
    CHECK(q.snr_db > 30.0);
    CHECK(q.ripple_db < 0.5);
    std::printf("  %s 模式 %d 和語音一起：%d/%d 幀，PSNR %.2f dB（直接收 %.2f），量到的訊雜比 %.1f dB（直接收 %.1f）；"
                "合成後峰值 %.2f、均方根 %.1f dBFS；語音比其餘的大 %.1f dB、各載波相差 %.2f dB\n",
                layout == DuplexLayout::Narrow ? "窄版面" : "標準版面", mode.id, through.good, frames, through.psnr, direct.psnr, through.snr, direct.snr,
                static_cast<double>(peak), 20.0 * std::log10(rms(air)), q.snr_db, q.ripple_db);
}

// 兩路互不干擾，音量照設定的比例
void test_isolation_and_levels() {
    const Mode& mode = *find_mode(3);
    const std::vector<float> picture = picture_signal(mode, 4);
    const std::vector<float> frames_only(picture.begin() + 3000, picture.end() - 6000);
    for (DuplexLayout layout : {DuplexLayout::Standard, DuplexLayout::Narrow}) {
        const std::vector<cf> voice = stand_in_voice(8000 * 4);
        // 只有語音：畫面那一路幾乎是空的，接收機不會交出任何幀；語音原樣回來
        const std::vector<float> voice_air = combine(layout, {}, voice);
        const Separated from_voice = separate(layout, voice_air);
        const double voice_power = mean_power(std::span<const float>(voice_air).subspan(4800, voice_air.size() - 9600));
        const double leak_to_picture = mean_power(from_voice.picture) / voice_power;
        CHECK(leak_to_picture < 1e-5);
        CHECK_EQ(receive(mode, from_voice.picture, duplex_band_shift_hz(layout)).reported, 0);
        const VoiceQuality alone = voice_quality(from_voice.voice, from_voice.voice.size() / 4, 128 * (from_voice.voice.size() / 2 / 128));
        CHECK(alone.snr_db > 45.0);
        CHECK(alone.ripple_db < 0.3);

        // 只有畫面：語音那一路幾乎是空的
        const std::vector<float> picture_air = combine(layout, frames_only, {});
        const Separated from_picture = separate(layout, picture_air);
        const double picture_power = mean_power(std::span<const float>(picture_air).subspan(4800, picture_air.size() - 9600));
        double voice_path = 0.0;
        for (cf v : from_picture.voice) voice_path += std::norm(v);
        // 語音那一路交出來的是解析訊號：同樣的東西，功率是實數訊號的兩倍
        const double leak_to_voice = 0.5 * voice_path / static_cast<double>(from_picture.voice.size()) / picture_power;
        CHECK(leak_to_voice < 3e-4);

        // 音量：預設兩路功率相同；語音調大 6 dB 就大 6 dB，合起來的峰值仍然在滿刻度以內
        CHECK_NEAR(10.0 * std::log10(voice_power / picture_power), 0.0, 0.4);
        // 語音回到接收端時的大小，和它在空中佔的份量一致（解析訊號的功率是實數的兩倍）
        CHECK_NEAR(alone.gain_db, 10.0 * std::log10(2.0 * voice_power / (kVoiceNominalRms * kVoiceNominalRms)), 0.1);
        for (float db : {6.0f, -6.0f}) {
            const std::vector<float> louder_voice = combine(layout, {}, voice, db);
            const std::vector<float> with_picture = combine(layout, frames_only, {}, db);
            const double v = mean_power(std::span<const float>(louder_voice).subspan(4800, louder_voice.size() - 9600));
            const double p = mean_power(std::span<const float>(with_picture).subspan(4800, with_picture.size() - 9600));
            CHECK_NEAR(10.0 * std::log10(v / p), db, 0.4);
            const std::vector<float> both = combine(layout, frames_only, voice, db);
            float peak = 0.0f;
            for (float s : both) peak = std::max(peak, std::fabs(s));
            CHECK(peak < 1.0f);
        }
        std::printf("  %s：只有語音時漏到畫面那一路 %.1f dB、只有畫面時漏到語音那一路 %.1f dB；語音單獨來回 %.1f dB\n",
                    layout == DuplexLayout::Narrow ? "窄版面" : "標準版面", 10.0 * std::log10(std::max(leak_to_picture, 1e-20)),
                    10.0 * std::log10(std::max(leak_to_voice, 1e-20)), alone.snr_db);
    }
}

// 「疊加」的版面：畫面與語音都不濾、不搬，這一層只換取樣率、調比例。
void test_overlay() {
    const DuplexLayout layout = DuplexLayout::Overlay;
    const Mode& mode = *find_mode(14);
    const int frames = 6;
    const std::vector<float> picture = picture_signal(mode, frames);
    const std::vector<float> frames_only(picture.begin() + 3000, picture.end() - 6000);
    const Reception direct = receive(mode, picture, 0.0);
    CHECK_EQ(direct.good, frames);

    // 只有畫面：原樣過去、原樣回來，只是小了一點（留給語音的份）。接收機收到的和直接收的一樣
    const std::vector<float> picture_air = combine(layout, picture, {});
    CHECK_NEAR(static_cast<double>(picture_air.size()), 4.0 * static_cast<double>(picture.size()), 4.0 * 1200.0 + 8.0);
    const Separated from_picture = separate(layout, picture_air);
    const Reception through = receive(mode, from_picture.picture, 0.0);
    CHECK_EQ(through.good, frames);
    CHECK_EQ(through.reported, frames);
    CHECK_NEAR(through.psnr, direct.psnr, 0.1);
    CHECK_NEAR(through.snr, direct.snr, 1.0);

    // 只有語音（替身）：送出去的就是它的實部；接收端語音那一路交出來的是解析訊號，各個單音原樣回來。
    // 畫面的接收機對它沒有反應
    const std::vector<cf> voice = stand_in_voice(8000 * 4, kLikeV1);
    const std::vector<float> voice_air = combine(layout, {}, voice);
    const Separated from_voice = separate(layout, voice_air);
    const std::size_t whole = kLikeV1.period * (from_voice.voice.size() / 2 / kLikeV1.period);
    const VoiceQuality alone = voice_quality(from_voice.voice, from_voice.voice.size() / 4, whole, kLikeV1);
    CHECK(alone.snr_db > 45.0);
    CHECK(alone.ripple_db < 0.3);
    CHECK_EQ(receive(mode, from_voice.picture, 0.0).reported, 0);

    // 音量：預設兩路功率相同；語音調大、調小 6 dB 就差 6 dB，合起來的峰值仍然在滿刻度以內
    const double picture_power = mean_power(std::span<const float>(combine(layout, frames_only, {})).subspan(4800, 4 * frames_only.size() - 9600));
    const double voice_power = mean_power(std::span<const float>(voice_air).subspan(4800, voice_air.size() - 9600));
    CHECK_NEAR(10.0 * std::log10(voice_power / picture_power), 0.0, 0.4);
    CHECK_NEAR(alone.gain_db, 10.0 * std::log10(2.0 * voice_power / (kLikeV1.rms * kLikeV1.rms)), 0.1);
    const std::vector<cf> noisy_voice = noise_like_voice(static_cast<std::size_t>(frames_only.size() * 2 / 3), 7);
    {
        double power = 0.0;
        float peak = 0.0f;
        for (cf v : noisy_voice) {
            power += std::norm(v);
            peak = std::max(peak, std::abs(v));
        }
        // 這個替身的大小和 RADE V1 給的差不多
        CHECK_NEAR(std::sqrt(power / static_cast<double>(noisy_voice.size())), kOverlayVoiceNominalRms, 0.06);
        CHECK(peak <= 1.0001f);
    }
    for (float db : {6.0f, 0.0f, -6.0f}) {
        const std::vector<float> v = combine(layout, {}, voice, db), p = combine(layout, frames_only, {}, db);
        CHECK_NEAR(10.0 * std::log10(mean_power(std::span<const float>(v).subspan(4800, v.size() - 9600)) /
                                     mean_power(std::span<const float>(p).subspan(4800, 4 * frames_only.size() - 9600))),
                   db, 0.4);
        float peak = 0.0f;
        for (float s : combine(layout, frames_only, noisy_voice, db)) peak = std::max(peak, std::fabs(s));
        CHECK(peak < 1.0f);
    }

    // 兩個一起、功率相同：語音對畫面來說是蓋住大半個頻帶、和它一樣強的雜訊，畫面還是解得出來（畫質掉幾 dB）；
    // 語音那一路交出來的訊號裡，語音與畫面各佔一半左右
    const std::vector<cf> long_voice = noise_like_voice(static_cast<std::size_t>(picture.size() * 2 / 3), 11);
    const std::vector<float> air = combine(layout, picture, long_voice);
    float peak = 0.0f;
    for (float v : air) peak = std::max(peak, std::fabs(v));
    CHECK(peak < 1.0f && peak > 0.5f);
    const Separated got = separate(layout, air);
    const Reception together = receive(mode, got.picture, 0.0);
    CHECK(together.pictures >= frames - 1);
    CHECK(together.good >= frames / 2);
    CHECK(together.psnr > direct.psnr - 7.0 && together.psnr < direct.psnr - 1.0);
    // 語音小 12 dB 時，畫面幾乎不受影響
    const Reception quiet_voice = receive(mode, separate(layout, combine(layout, picture, long_voice, -12.0f)).picture, 0.0);
    CHECK_EQ(quiet_voice.good, frames);
    CHECK(quiet_voice.psnr > direct.psnr - 2.0);
    std::printf("  疊加、模式 %d：只有畫面 %d/%d 幀、PSNR %.2f dB（直接收 %.2f）；語音（替身）單獨來回 %.1f dB、各載波相差 %.2f dB；\n"
                "    和一樣強的語音一起：%d 張畫面（標頭正確 %d）、PSNR %.2f dB、量到的訊雜比 %.1f dB、合成後峰值 %.2f；語音小 12 dB：%d 幀、%.2f dB\n",
                mode.id, through.good, frames, through.psnr, direct.psnr, alone.snr_db, alone.ripple_db, together.pictures, together.good, together.psnr,
                together.snr, static_cast<double>(peak), quiet_voice.good, quiet_voice.psnr);

    // 接收機自己看得出頻帶兩端（語音蓋不到的載波）比較乾淨（RxTuning::quiet_carriers）：標頭因此比較常解得出來。
    // 多送幾幀才看得出差別；只有畫面的時候開不開都一樣
    const int many = 24;
    const std::vector<float> long_picture = picture_signal(mode, many);
    const std::vector<float> long_air = combine(layout, long_picture, noise_like_voice(static_cast<std::size_t>(long_picture.size() * 2 / 3), 5));
    const std::vector<float> long_got = separate(layout, long_air).picture;
    const Reception with_rule = receive(mode, long_got, 0.0, true), without_rule = receive(mode, long_got, 0.0, false);
    CHECK(with_rule.pictures >= many - 1 && without_rule.pictures >= many - 1);
    CHECK(with_rule.good >= without_rule.good);
    const Reception plain_with = receive(mode, long_picture, 0.0, true), plain_without = receive(mode, long_picture, 0.0, false);
    CHECK_EQ(plain_with.good, many);
    CHECK_EQ(plain_without.good, many);
    CHECK_NEAR(plain_with.psnr, plain_without.psnr, 0.01);
    std::printf("    %d 幀、和一樣強的語音一起：標頭正確 %d 張（不看兩端比較乾淨的載波時 %d 張）\n", many, with_rule.good, without_rule.good);
}

// 把一段 48 kHz 的音訊當成接收端的取樣時脈快了 ppm（負值是慢了）重新取樣：四點內插，訊號都在 3 kHz 以下，誤差很小
std::vector<float> with_clock_offset(std::span<const float> x, double ppm) {
    std::vector<float> y;
    const double step = 1.0 / (1.0 + ppm * 1e-6);
    for (double t = 1.0; t + 2.0 < static_cast<double>(x.size()); t += step) {
        const std::size_t i = static_cast<std::size_t>(t);
        const double f = t - static_cast<double>(i);
        const double a = x[i - 1], b = x[i], c = x[i + 1], d = x[i + 2];
        y.push_back(static_cast<float>(b + 0.5 * f * (c - a + f * (2.0 * a - 5.0 * b + 4.0 * c - d + f * (3.0 * (b - c) + d - a)))));
    }
    return y;
}

// 兩邊音效卡的取樣時脈差 200 ppm：畫面移過頻率，所以整個頻帶會跟著轉，沒有散佈導頻的幀頭尾之間轉超過半圈。
// 接收機知道移了多少（band_shift_hz）就接得起來。
void test_clock_offset() {
    const Mode& mode = *find_mode(5);  // 2.29 秒的幀，沒有散佈導頻
    const int frames = 2;
    const std::vector<float> picture = picture_signal(mode, frames);
    struct Case {
        DuplexLayout layout;
        double ppm;
    };
    for (const Case& c : {Case{DuplexLayout::Standard, 200.0}, Case{DuplexLayout::Narrow, -200.0}}) {
        const double seconds = static_cast<double>(picture.size()) / kDuplexPictureRate * duplex_slow_factor(c.layout);
        const std::vector<float> air = combine(c.layout, picture, stand_in_voice(static_cast<std::size_t>(seconds * kDuplexVoiceRate)));
        const Separated got = separate(c.layout, with_clock_offset(air, c.ppm));
        const Reception told = receive(mode, got.picture, duplex_band_shift_hz(c.layout));
        CHECK_EQ(told.good, frames);
        const Reception untold = receive(mode, got.picture, 0.0);
        const VoiceQuality q = voice_quality(got.voice, got.voice.size() / 4, 128 * 16);  // 時脈有差，單音的頻率也跟著偏一點：只看一小段
        CHECK(q.snr_db > 20.0);
        std::printf("  %s 模式 %d、時脈差 %+.0f ppm：%d/%d 幀，PSNR %.2f dB（接收機不知道移過頻率時 %d/%d 幀）\n",
                    c.layout == DuplexLayout::Narrow ? "窄版面" : "標準版面", mode.id, c.ppm, told.good, frames, told.psnr, untold.good, frames);
    }
}

// 分幾次給、分幾次取，和一次做完完全相同；沒有東西可以取的時候是靜音
void test_streaming() {
    Rng rng(23);
    std::vector<float> picture(6000);
    for (float& v : picture) v = static_cast<float>(0.2 * rng.gauss());
    const std::vector<cf> voice = stand_in_voice(4000);
    for (DuplexLayout layout : {DuplexLayout::Standard, DuplexLayout::Narrow, DuplexLayout::Overlay}) {
        DuplexTx whole(layout), pieces(layout);
        std::vector<float> none(64, 1.0f);
        whole.read(none);  // 還沒有東西：靜音
        CHECK(std::all_of(none.begin(), none.end(), [](float v) { return v == 0.0f; }));
        whole.push_picture(picture);
        whole.push_voice(voice);
        const std::size_t picture_ready = whole.pending_picture(), voice_ready = whole.pending_voice();
        CHECK_NEAR(static_cast<double>(picture_ready), 6000.0 * 4.0 * duplex_slow_factor(layout), 8.0);
        CHECK_NEAR(static_cast<double>(voice_ready), 4000.0 * 6.0, 8.0);
        std::vector<float> a(std::max(picture_ready, voice_ready) + 100);  // 多取的部分是靜音
        whole.read(a);
        CHECK_EQ(static_cast<long long>(whole.pending_picture()), 0);
        for (std::size_t pos = 0; pos < picture.size();) {
            const std::size_t n = std::min<std::size_t>(1 + rng.below(700), picture.size() - pos);
            pieces.push_picture(std::span<const float>(picture).subspan(pos, n));
            pos += n;
        }
        for (std::size_t pos = 0; pos < voice.size();) {
            const std::size_t n = std::min<std::size_t>(1 + rng.below(300), voice.size() - pos);
            pieces.push_voice(std::span<const cf>(voice).subspan(pos, n));
            pos += n;
        }
        std::vector<float> b(a.size());
        for (std::size_t pos = 0; pos < b.size();) {
            const std::size_t n = std::min<std::size_t>(1 + rng.below(5000), b.size() - pos);
            pieces.read(std::span<float>(b).subspan(pos, n));
            pos += n;
        }
        CHECK(a == b);
        CHECK(std::all_of(a.end() - 50, a.end(), [](float v) { return v == 0.0f; }));
        // reset 之後和新的一樣
        whole.reset();
        CHECK_EQ(static_cast<long long>(whole.pending_voice()), 0);
        whole.push_picture(picture);
        whole.push_voice(voice);
        std::vector<float> again(a.size());
        whole.read(again);
        CHECK(a == again);

        // 接收端：一次給完，和一個一個、幾個幾個地給
        const Separated one = separate(layout, a, a.size());
        for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{481}}) {
            const Separated other = separate(layout, a, chunk);
            CHECK(one.picture == other.picture);
            CHECK(one.voice == other.voice);
        }
        CHECK_NEAR(static_cast<double>(one.picture.size()), static_cast<double>(a.size()) / (4.0 * duplex_slow_factor(layout)), 2.0);
        CHECK_NEAR(static_cast<double>(one.voice.size()), static_cast<double>(a.size()) / 6.0, 2.0);
        // 壞掉的取樣不會擴散
        std::vector<float> bad = a;
        bad[1000] = std::nanf("");
        bad[2000] = HUGE_VALF;
        const Separated cleaned = separate(layout, bad);
        CHECK(std::all_of(cleaned.picture.begin(), cleaned.picture.end(), [](float v) { return std::isfinite(v); }));
        CHECK(std::all_of(cleaned.voice.begin(), cleaned.voice.end(), [](cf v) { return std::isfinite(v.real()) && std::isfinite(v.imag()); }));
    }
}

}  // namespace

int main() {
    test_numbers();
    test_resampler();
    test_band_mover();
    test_streaming();
    if (!have_models()) {
        std::printf("  找不到權重檔，略過要收發畫面的測試\n");
        return check::finish("duplex");
    }
    test_isolation_and_levels();
    test_together(DuplexLayout::Standard, 3, 3);   // 最快的：96×72、沒有散佈導頻
    test_together(DuplexLayout::Narrow, 3, 3);
    test_together(DuplexLayout::Standard, 5, 2);   // 192×144、沒有散佈導頻
    test_together(DuplexLayout::Narrow, 14, 3);    // 有散佈導頻的
    test_overlay();
    test_clock_offset();
    return check::finish("duplex");
}
