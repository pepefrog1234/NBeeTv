#include "receiver.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>

#include "analog_frame.hpp"
#include "digital_frame.hpp"
#include "fft.hpp"
#include "frame_map.hpp"

namespace nbtv {

namespace {

// S0 的滑動相關：S0 連循環字首共 320 點，是 128 點週期的 2.5 個週期，
// 所以相隔 128 點的乘積可以累加 192 項。
constexpr int kScLag = kFftSize / 2;
constexpr int kScLength = kSymbolSamples - kScLag;  // 192
constexpr int kScPeakHold = 160;                    // 峰值之後這麼多點沒有更高的值才定案

constexpr int kAcquireBackoff = 24;   // 捕獲時 FFT 視窗比估計的起點提前的取樣數（粗定時誤差大）
constexpr int kTrackBackoff = 16;     // 追蹤時的提前量：留給定時誤差、取樣時脈漂移與濾波器的前緣
// 追蹤時確認下一幀的前導符號：延遲視窗（長 32 點）起點的範圍，相對於 FFT 視窗起點。第一條路徑理應在 +16，
// 這個範圍容許它前後各差 40 點——process_frame 的定時修正處理得了這麼多。差得更遠的（對方的音效卡掉了一小段、
// 或是另一次發射剛好在附近開始）不算確認到，交給搜尋重新捕獲，那邊的定時不受這個限制。
constexpr int kTrackDetectFirst = -24;
constexpr int kTrackDetectLast = 24;
constexpr int kTailSegment = 18;      // 解出來的幀：以這麼多個符號為一段，從尾端往回看導頻在不在
constexpr double kSolidScThreshold = 0.15;  // 收訊中找「很明確的前導符號」時 S0 滑動相關的門檻（訊雜比 −2 dB 的 S0 約 0.15）
constexpr double kS1MinScore = 1.5;   // 前導符號的判定：S1 單獨的分數至少要這麼高（只有雜訊時約為 1）
constexpr double kS1PeakRatio = 0.5;  // ……而且至少是 S1 在所有模式、所有延遲裡最高分數的這個比例
constexpr double kS1BalanceRatio = 0.25;  // ……也至少是 S0 單獨的分數的這個比例
constexpr double kMaxS1Sparsity = 0.8;    // S1 的能量集中在少數載波的程度（見 sparsity）超過這個值就不是 S1
constexpr int kVerifySymbols = 12;    // 捕獲後用酬載開頭這麼多個符號的導頻做驗證
constexpr int kPresenceSymbols = 36;  // 沒確認到前導符號的幀：收到這麼多個酬載符號後，檢查導頻是否還在
constexpr double kPresenceThreshold = 0.2;
constexpr double kCfoRedoHz = 0.2;    // 殘餘頻偏超過這個值就重新解調

constexpr std::int64_t kBufferSlack = 1 << 15;

const Fft& fft256() {
    static const Fft fft(kFftSize);
    return fft;
}

const Fft& fft128() {
    static const Fft fft(kFftSize / 2);
    return fft;
}

const std::array<int, 5>& acquire_shifts() {
    static const std::array<int, 5> shifts = {0, -2, 2, -4, 4};
    return shifts;
}

// 偵測用的幅度上限：一個符號裡各載波幅度中位數的 kLimitFactor 倍。
// 判定前導符號與導頻在不在的統計量，都假設能量大致平均分布在各個載波上。類比模式的畫面如果能量集中在
// 少數幾個係數（測試圖、色條），那幾個格點會比其他的大上幾十倍，少數幾項就足以左右整個統計量，
// 於是在酬載裡誤判出前導符號或導頻。把過大的值壓到上限（只改大小、不改相位）之後就不會；
// 正常的訊號（含選擇性衰落）幅度很少超過中位數的 2.5 倍，幾乎不受影響。只用在偵測，不用在解調。
constexpr float kLimitFactor = 2.5f;

float clip_level(const cf* values, int count, int stride = 1) {
    assert(count <= kCarriers);
    std::array<float, kCarriers> mag;
    for (int i = 0; i < count; ++i) mag[static_cast<std::size_t>(i)] = std::abs(values[i * stride]);
    std::nth_element(mag.begin(), mag.begin() + count / 2, mag.begin() + count);
    return kLimitFactor * mag[static_cast<std::size_t>(count / 2)];
}

// 一個符號的能量是不是集中在少數幾個載波上：先不算最強的兩個（可能是單頻干擾），
// 回傳接下來最強的 9 個佔其餘能量的比例。S1 的 49 個載波功率相同，就算經過選擇性衰落、淹在雜訊裡，
// 這個比例也只有五成左右；類比模式的畫面如果幾乎沒有內容，酬載符號只剩導頻那八九個載波，比例會超過九成。
// 那種符號每 128 點重複（導頻每 6 格一個），S0 的滑動相關會把它當成候選，導頻的 PRBS 又可能湊巧和某個模式的
// S1 序列有幾分相似，所以要另外擋掉。
double sparsity(const cf* values, int count) {
    assert(count <= kCarriers && count > 11);
    std::array<float, kCarriers> power;
    double total = 0.0;
    for (int i = 0; i < count; ++i) {
        power[static_cast<std::size_t>(i)] = std::norm(values[i]);
        total += power[static_cast<std::size_t>(i)];
    }
    std::partial_sort(power.begin(), power.begin() + 11, power.begin() + count, std::greater<float>());
    const double rest = total - power[0] - power[1];
    double top = 0.0;
    for (int i = 2; i < 11; ++i) top += power[static_cast<std::size_t>(i)];
    return rest > 0.0 ? top / rest : 0.0;
}

cf limited(cf v, float level) {
    const float m = std::abs(v);
    return m > level ? v * (level / m) : v;
}

// 同一格相鄰兩個導頻（相隔 3 個符號）是否相干：有訊號而且位置對的時候接近 S/(S+N)，
// 只有雜訊時接近 0（約 1/sqrt(導頻對數)）。只看第 first 到 last−1 個酬載符號。
// 每一對符號（第 j 個與第 j+3 個）先各自算出正規化的相關值，再對所有的符號對取平均：每一對的份量相同，
// 不會被少數幾個特別強的符號左右（例如檢查的範圍裡大半是雜訊、最後幾個符號卻是另一次發射的開頭），
// 完全沒有訊號的符號（數位的靜音）也照樣算一對、相關值是 0。
double pilot_coherence(const SymbolGrid& grid, int first, int last) {
    cd total(0, 0);
    int pairs = 0;
    for (int j = first; j + 3 < last; ++j) {
        const int ra = j + kPreambleSymbols, rb = ra + 3;
        const float la = clip_level(&grid.at(ra, 0), kCarriers), lb = clip_level(&grid.at(rb, 0), kCarriers);
        cd acc(0, 0);
        double ea = 0.0, eb = 0.0;
        for (int c = 0; c < kCarriers; ++c) {
            if (!is_pilot(j, kFirstBin + c)) continue;  // 第 j 與第 j+3 個符號的導頻在同樣的格子上
            const cd a = limited(grid.at(ra, c), la);
            const cd b = limited(grid.at(rb, c), lb);
            acc += b * std::conj(a);
            ea += std::norm(a);
            eb += std::norm(b);
        }
        ++pairs;
        if (ea > 0.0 && eb > 0.0) total += acc / std::sqrt(ea * eb);
    }
    return pairs > 0 ? std::abs(total) / pairs : 0.0;
}

}  // namespace

Receiver::Receiver() { buf_.reserve(1 << 17); }

void Receiver::reset() {
    analytic_.reset();
    buf_.clear();
    base_ = 0;
    total_in_ = 0;
    level_ = 0.0;
    frames_.clear();
    frames_decoded_ = 0;
    frames_failed_ = 0;
    mode_ = nullptr;
    snr_valid_ = false;
    sync_quality_ = 0.0;
    analog_gain_code_ = -1;
    last_ok_ = false;
    last_complete_ = false;
    unconfirmed_run_ = 0;
    rescan_from_ = 0;
    frame_evidence_ = 0.0;
    pending_start_ = -1;
    enter_search(0);
}

void Receiver::enter_search(std::int64_t from) {
    state_ = State::Search;
    snr_valid_ = false;
    start_scan(from, INT64_MAX);
}

// 從 from 開始找 S0，到 limit 為止（搜尋狀態沒有上限；收訊時一邊收一邊找，只找到目前這一幀結束之前）。
void Receiver::start_scan(std::int64_t from, std::int64_t limit) {
    pending_start_ = -1;
    scan_limit_ = limit;
    scan_pos_ = std::max(from, base_);
    sc_valid_ = false;
    sc_background_ = cd(0, 0);
    peak_active_ = false;
    cand_ready_ = false;
}

void Receiver::push(std::span<const float> samples) {
    if (samples.empty()) return;
    // 輸入音量：時間常數約 0.1 秒的指數平均
    const double alpha = 1.0 / (0.1 * kSampleRate);
    for (float s : samples) level_ += alpha * (static_cast<double>(s) * s - level_);
    analytic_.process(samples, buf_);
    total_in_ += static_cast<std::int64_t>(samples.size());
    while (step()) {
    }
    trim();
}

bool Receiver::poll(RxFrame& out) {
    if (frames_.empty()) return false;
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

RxStatus Receiver::status() const {
    RxStatus s;
    // 剛捕獲、還沒確認導頻存在之前不算同步，免得雜訊造成的誤判讓畫面閃一下
    s.synced = state_ != State::Search && mode_ != nullptr && (presence_checked_ || state_ == State::Confirm);
    s.input_level_db = static_cast<float>(10.0 * std::log10(std::max(level_, 1e-12)));
    s.frames_decoded = frames_decoded_;
    s.frames_failed = frames_failed_;
    s.sync_quality = static_cast<float>(sync_quality_);
    if (s.synced) {
        s.mode_id = mode_->id;
        s.snr_db = snr_db_;
        s.snr_valid = snr_valid_;
        s.freq_offset_hz = static_cast<float>(cfo_ * kSampleRate);
        s.symbols_total = mode_->frame_symbols();
        const std::int64_t done = (end() - t0_) / kSymbolSamples;
        s.symbols_done = static_cast<int>(std::clamp<std::int64_t>(done, 0, s.symbols_total));
    }
    return s;
}

void Receiver::trim() {
    // 留下搜尋位置或目前這一幀之前的一小段，其餘丟掉；累積到一定量才真的搬動記憶體。
    std::int64_t keep = (state_ == State::Search ? scan_pos_ : t0_) - 2048;
    // 追蹤中：保留可能要回頭重新搜尋的那一段，以及正在一邊收一邊找 S0 的位置
    if (state_ != State::Search) {
        keep = std::min(keep, rescan_from_ - 2048);
        if (state_ == State::Receive) keep = std::min(keep, std::min(scan_pos_, cand_ready_ ? cand_pos_ : scan_pos_) - 2048);
        if (pending_start_ >= 0) keep = std::min(keep, pending_start_ - 2048);
    }
    if (keep - base_ > kBufferSlack) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(keep - base_));
        base_ = keep;
    }
}

bool Receiver::step() {
    switch (state_) {
        case State::Search:
            return search_step();
        case State::Receive: {
            // 一邊收一邊繼續找 S0。前導符號的證據不夠強的幀只是暫定的（沒確認到前導符號、只是假設對方還在送；
            // 或者分數剛好過門檻，有可能是雜訊）：找到更確定的前導符號就改收那一幀。證據很強的幀不會被取代，
            // 但裡面如果又出現一個很明確的前導符號，表示對方中途停了、換成另一次發射，收完這一幀就直接去那裡。
            if (search_step()) return true;
            // 先收一小段，看導頻在不在，不在就放棄，不必等整幀。
            if (!presence_checked_) {
                const int rows = std::min(mode_->frame_symbols(), kPreambleSymbols + kPresenceSymbols);
                if (end() < t0_ + static_cast<std::int64_t>(rows) * kSymbolSamples + kCyclicPrefix) return false;
                presence_checked_ = true;
                if (!pilots_present(t0_, cfo_, *mode_)) {
                    if (trace_) std::fprintf(stderr, "[rx] 幀起點 %lld 之後沒有導頻，放棄\n", static_cast<long long>(t0_));
                    mode_ = nullptr;
                    enter_search(rescan_from_);
                    return true;
                }
            }
            if (end() < t0_ + mode_->frame_samples() + 2 * kCyclicPrefix) return false;
            process_frame();
            return true;
        }
        case State::Confirm: {
            // 上一幀沒解出來（或沒送完）的話，還要用酬載開頭的導頻驗證（和捕獲時一樣），所以多等幾個符號
            const int symbols = last_complete_ ? kPreambleSymbols : kPreambleSymbols + kVerifySymbols;
            if (end() < t0_ + static_cast<std::int64_t>(symbols) * kSymbolSamples + 3 * kCyclicPrefix) return false;
            confirm_next();
            return true;
        }
    }
    return false;
}

bool Receiver::search_step() {
    // 正在收一個證據很強的幀時，只需要留意同樣很明確的前導符號，那種前導符號的相關值很高；
    // 門檻拉高之後，酬載裡幾乎不會有候選位置要檢查，不然收訊時的運算量會多出好幾倍。
    const double threshold = (state_ == State::Receive && !tentative()) ? kSolidScThreshold : tuning_.sc_threshold;
    for (;;) {
        if (cand_ready_) {
            // 需要 S0、S1 與驗證用的酬載符號，外加定時修正的餘裕
            if (end() < cand_pos_ + (kPreambleSymbols + kVerifySymbols) * kSymbolSamples + 3 * kCyclicPrefix) return false;
            cand_ready_ = false;
            if (try_acquire(cand_pos_, cand_p_, cand_metric_)) return true;
        }
        if (scan_pos_ >= scan_limit_ || scan_pos_ + kScLength + kScLag > end()) return false;

        const std::int64_t d = scan_pos_;
        if (!sc_valid_ || sc_age_ >= 4096) {
            // 從頭算一次，避免遞迴更新累積誤差
            sc_p_ = cd(0, 0);
            sc_e1_ = sc_e2_ = 0.0;
            for (int m = 0; m < kScLength; ++m) {
                const cd a = sample(d + m), b = sample(d + m + kScLag);
                sc_p_ += std::conj(a) * b;
                sc_e1_ += std::norm(a);
                sc_e2_ += std::norm(b);
            }
            sc_valid_ = true;
            sc_age_ = 0;
        } else {
            // 由 d−1 的結果更新：去掉最舊的一項，加入最新的一項
            const cd a0 = sample(d - 1), b0 = sample(d - 1 + kScLag);
            const cd a1 = sample(d - 1 + kScLength), b1 = sample(d - 1 + kScLength + kScLag);
            sc_p_ += std::conj(a1) * b1 - std::conj(a0) * b0;
            sc_e1_ += std::norm(a1) - std::norm(a0);
            sc_e2_ += std::norm(b1) - std::norm(b0);
            ++sc_age_;
        }
        // 頻帶內若有固定的載波（或連續發射時酬載裡的導頻），相關值會有一個不隨時間變的成分，
        // 它既抬高門檻下的底，也會把頻偏估計帶偏。以約 0.7 秒的指數平均估出來扣掉；S0 只有 27 ms，幾乎不受影響。
        sc_background_ += (sc_p_ - sc_background_) * (1.0 / 8192.0);
        // 訊號突然變小（一次發射剛結束、後面是完全的靜音）時，先前累積的平均值還來不及降下來，
        // 會比現在的能量大得多。固定的干擾造成的相關值不可能超過 sqrt(E1·E2)，超過的部分不扣。
        const double energy = std::max(sc_e1_, 0.0) * std::max(sc_e2_, 0.0);
        cd background = sc_background_;
        if (std::norm(background) > energy) background *= std::sqrt(energy / std::norm(background));
        const cd corr = sc_p_ - background;
        const double metric = std::norm(corr) / (energy + 1e-30);

        if (!peak_active_) {
            if (metric > threshold) {
                peak_active_ = true;
                peak_metric_ = metric;
                peak_pos_ = d;
                peak_p_ = corr;
            }
        } else if (metric > peak_metric_) {
            peak_metric_ = metric;
            peak_pos_ = d;
            peak_p_ = corr;
        } else if (d - peak_pos_ >= kScPeakHold) {
            peak_active_ = false;
            cand_ready_ = true;
            cand_pos_ = peak_pos_;
            cand_p_ = peak_p_;
            cand_metric_ = peak_metric_;
        }
        ++scan_pos_;
    }
}

// 取 256 點、修正頻偏後做 FFT。頻偏的相位以取樣編號為準，所以各個視窗之間相位連續。
void Receiver::fft_window(std::int64_t start, double cfo, cf* out) const {
    assert(start >= base_ && start + kFftSize <= end());
    const double step = -kTwoPi * cfo;
    // 相位只取小數部分，避免取樣編號很大時損失精度
    double phase0 = std::fmod(step * static_cast<double>(start % (1LL << 40)), kTwoPi);
    cd rot = std::polar(1.0, phase0);
    const cd inc = std::polar(1.0, step);
    const cf* in = &buf_[static_cast<std::size_t>(start - base_)];
    for (int n = 0; n < kFftSize; ++n) {
        out[n] = in[n] * cf(rot);
        rot *= inc;
    }
    fft256().forward(out);
}

// 檢查某個位置是不是前導符號，並判定模式與整數頻偏。
// 做法：把 S0、S1 各自除以已知序列得到通道的頻率響應，轉到延遲域。假設對了的話，能量會集中在
// 一小段延遲內（通道的延遲擴散有限）；假設錯了或只有雜訊，能量會平均散開。
// 分數 = 視窗內的能量密度 ÷ 視窗外的能量密度（各自除以自由度），只有雜訊時約為 1。
// s0_window 是 S0 的 FFT 視窗起點，S1 的視窗在一個符號之後。
Receiver::PreambleCheck Receiver::check_preamble(std::int64_t s0_window, double cfo, std::span<const int> shifts,
                                                 int expect_mode, int detect_first, int detect_last) const {
    constexpr int kHalf = kFftSize / 2;
    const int kDetectWindow = tuning_.detect_window;
    const int kDetectFirst = detect_first, kDetectLast = detect_last;
    // 視窗內外的自由度：S1 有 49 個載波、S0 有 25 個，視窗佔延遲範圍的比例分別是 W/256 與 W/128
    const double kDofIn = kCarriers * static_cast<double>(kDetectWindow) / kFftSize +
                          kS0Carriers * static_cast<double>(kDetectWindow) / kHalf;
    const double kDofOut = kCarriers + kS0Carriers - kDofIn;
    const double kDofIn1 = kCarriers * static_cast<double>(kDetectWindow) / kFftSize;  // 只算 S1 的部分
    const double kDofOut1 = kCarriers - kDofIn1;
    const double kDofIn0 = kS0Carriers * static_cast<double>(kDetectWindow) / kHalf;   // 只算 S0 的部分
    const double kDofOut0 = kS0Carriers - kDofIn0;

    cf y0[kFftSize], y1[kFftSize];
    fft_window(s0_window, cfo, y0);
    fft_window(s0_window + kSymbolSamples, cfo, y1);

    PreambleCheck best;
    std::array<double, kHalf> p0{};
    std::array<double, kFftSize> p1{};
    cf a[kHalf], b[kFftSize];
    for (int shift : shifts) {
        if (sparsity(&y1[kFirstBin + shift], kCarriers) > kMaxS1Sparsity) continue;  // 這不是 S1（見 sparsity）
        const float level0 = clip_level(&y0[kFirstBin + shift], kS0Carriers, 2);
        const float level1 = clip_level(&y1[kFirstBin + shift], kCarriers);
        // S0：偶數格，延遲以 128 為週期
        std::fill(a, a + kHalf, cf(0, 0));
        double e0 = 0.0;
        for (int i = 0; i < kS0Carriers; ++i) {
            const int bin = kFirstBin + 2 * i;
            const cf v = limited(y0[bin + shift], level0) * std::conj(s0_value(bin)) * (1.0f / kS0Amplitude);
            a[bin / 2] = v;
            e0 += std::norm(v);
        }
        fft128().inverse(a);
        for (int t = 0; t < kHalf; ++t) p0[static_cast<std::size_t>(t)] = std::norm(a[t]) / kHalf;

        // 這個整數頻偏下，每個模式各自的分數；等全部算完才知道 S1 在哪個模式、哪個延遲最集中
        struct Hypothesis {
            double score = 0.0, score_s1 = 0.0, score_s0 = 0.0;
        };
        std::array<Hypothesis, kModes.size()> hyp{};
        double s1_peak = 0.0;  // S1 單獨的分數在所有模式、所有延遲裡的最大值
        for (std::size_t mi = 0; mi < kModes.size(); ++mi) {
            const Mode& m = kModes[mi];
            std::fill(b, b + kFftSize, cf(0, 0));
            double e1 = 0.0;
            for (int c = 0; c < kCarriers; ++c) {
                const int bin = kFirstBin + c;
                const cf v = limited(y1[bin + shift], level1) * (s1_value(m.id, bin) > 0 ? 1.0f : -1.0f);
                b[bin] = v;
                e1 += std::norm(v);
            }
            fft256().inverse(b);
            for (int t = 0; t < kFftSize; ++t) p1[static_cast<std::size_t>(t)] = std::norm(b[t]) / kFftSize;
            auto s1_tap = [&](int t) { return p1[static_cast<std::size_t>((t % kFftSize + kFftSize) % kFftSize)]; };
            auto s1_score = [&](double win1) { return (win1 / kDofIn1) / (std::max(e1 - win1, 1e-30) / kDofOut1); };

            // 視窗起點在 [kDetectFirst, kDetectLast] 之間滑動，取能量最大的位置
            auto tap = [&](int t) { return s1_tap(t) + p0[static_cast<std::size_t>((t % kHalf + kHalf) % kHalf)]; };
            double win = 0.0;
            for (int t = 0; t < kDetectWindow; ++t) win += tap(kDetectFirst + t);
            double best_win = win;
            int best_start = kDetectFirst;
            for (int start = kDetectFirst + 1; start <= kDetectLast; ++start) {
                win += tap(start + kDetectWindow - 1) - tap(start - 1);
                if (win > best_win) {
                    best_win = win;
                    best_start = start;
                }
            }
            const double outside = std::max(e0 + e1 - best_win, 1e-30);
            hyp[mi].score = (best_win / kDofIn) / (outside / kDofOut);
            // 同一個延遲視窗裡，只看 S1 的分數，以及只看 S0 的分數
            double win1 = 0.0;
            for (int t = 0; t < kDetectWindow; ++t) win1 += s1_tap(best_start + t);
            hyp[mi].score_s1 = s1_score(win1);
            const double win0 = best_win - win1;
            hyp[mi].score_s0 = (win0 / kDofIn0) / (std::max(e0 - win0, 1e-30) / kDofOut0);
            // S1 在整個延遲範圍（256 點繞一圈）裡最集中的地方
            double any = 0.0;
            for (int t = 0; t < kDetectWindow; ++t) any += s1_tap(t);
            double any_best = any;
            for (int start = 1; start < kFftSize; ++start) {
                any += s1_tap(start + kDetectWindow - 1) - s1_tap(start - 1);
                any_best = std::max(any_best, any);
            }
            s1_peak = std::max(s1_peak, s1_score(any_best));
        }
        // S0 以 128 點為週期：位置差了 128 點的前導符號，S0 的部分照樣集中，光靠它分不出來。
        // S1 沒有這個模糊。所以要求 S1 單獨也要集中在同一個延遲視窗裡，和 S0 的集中程度不能差太多
        // （兩者功率相同、走同一個通道），而且不能有別的模式或別的延遲比它集中得多——
        // 那表示這裡確實有前導符號，但不是這個假設說的位置或模式。
        const double s1_floor = std::max(kS1MinScore, kS1PeakRatio * s1_peak);
        for (std::size_t mi = 0; mi < kModes.size(); ++mi) {
            if (hyp[mi].score_s1 < s1_floor || hyp[mi].score_s1 < kS1BalanceRatio * hyp[mi].score_s0) continue;
            if (hyp[mi].score > best.score) {
                best.score = hyp[mi].score;
                best.score_s1 = hyp[mi].score_s1;
                best.mode_id = kModes[mi].id;
                best.shift = shift;
            }
            if (shift == 0 && kModes[mi].id == expect_mode) best.score_mode = hyp[mi].score;
        }
    }
    return best;
}

// 由 S1 的通道估計求最強路徑與第一條路徑相對於視窗起點的延遲（取樣數）。
Receiver::PathDelays Receiver::s1_path_delays(std::int64_t s1_window, double cfo, int mode_id) const {
    cf y[kFftSize];
    fft_window(s1_window, cfo, y);
    cf x[kFftSize];
    std::fill(x, x + kFftSize, cf(0, 0));
    for (int c = 0; c < kCarriers; ++c) {
        const float hann = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * (c + 1) / (kCarriers + 1)));
        x[kFirstBin + c] = y[kFirstBin + c] / s1_value(mode_id, kFirstBin + c) * hann;
    }
    fft256().inverse(x);
    std::array<double, kFftSize> profile{};
    for (int t = 0; t < kFftSize; ++t) profile[static_cast<std::size_t>(t)] = std::norm(x[t]);
    const int peak = static_cast<int>(std::max_element(profile.begin(), profile.end()) - profile.begin());
    std::array<double, kFftSize> sorted = profile;
    std::nth_element(sorted.begin(), sorted.begin() + kFftSize / 2, sorted.end());
    // 較早的路徑要比雜訊底高出一大截才算數（單一符號的估計很吵，門檻低了會被雜訊騙到提前幾十個取樣）
    const double threshold = std::max(0.2 * profile[static_cast<std::size_t>(peak)], 12.0 * sorted[kFftSize / 2]);
    int first = peak;
    for (int back = 64; back > 0; --back) {
        const int t = ((peak - back) % kFftSize + kFftSize) % kFftSize;
        if (profile[static_cast<std::size_t>(t)] >= threshold) {
            first = peak - back;
            break;
        }
    }
    constexpr double kHalfRange = kFftSize / 2.0;
    PathDelays out;
    out.peak = peak >= kHalfRange ? peak - kFftSize : peak;
    out.first = first + (first == peak ? 0.0 : 5.0);  // 加窗後主瓣在門檻處提前約 5 個取樣
    if (out.first >= kHalfRange) out.first -= kFftSize;
    return out;
}

bool Receiver::try_acquire(std::int64_t s0_start, cd corr, double metric) {
    // 小數頻偏：相隔 128 點的相位差。可分辨 ±46.875 Hz，其餘（93.75 Hz 的整數倍）交給 S1。
    const double eps = std::arg(corr) / (kTwoPi * kScLag);
    // 弱訊號時滑動相關的峰值位置可以偏掉一百多個取樣，超出 check_preamble 能涵蓋的範圍，
    // 所以在峰值前後 80、160 個取樣也各檢查一次，取分數最高的；已經很明確時就不必再試。
    PreambleCheck chk;
    std::int64_t w0 = 0;
    for (int offset : {0, -80, 80, -160, 160}) {
        const std::int64_t w = s0_start + offset + kCyclicPrefix - kAcquireBackoff;
        if (w < base_ || w + kSymbolSamples + kFftSize > end()) continue;
        const PreambleCheck one = check_preamble(w, eps, acquire_shifts(), -1, tuning_.detect_first, tuning_.detect_last);
        if (one.score > chk.score) {
            chk = one;
            w0 = w;
        }
        if (chk.score >= 2.0 * tuning_.acquire_score) break;
    }
    if (chk.mode_id < 0) return false;
    if (trace_) {
        std::fprintf(stderr, "[rx] 候選 S0 @%lld：相關 %.3f、小數頻偏 %+.1f Hz → 模式 %d、整數頻偏 %+d 格、分數 %.2f（S1 %.2f）\n",
                     static_cast<long long>(s0_start), metric, eps * kSampleRate, chk.mode_id, chk.shift, chk.score, chk.score_s1);
    }
    if (chk.score < tuning_.acquire_score) return false;
    // 正在收一幀：暫定的幀，新的前導符號要比它更確定才取代它；證據很強的幀，只理會同樣很明確的前導符號
    const bool receiving = state_ == State::Receive;
    if (receiving && (tentative() ? chk.score <= frame_evidence_ : chk.score < 2.0 * tuning_.acquire_score)) return false;

    const Mode* mode = find_mode(chk.mode_id);
    assert(mode != nullptr);
    double cfo = eps + static_cast<double>(chk.shift) / kFftSize;
    const std::int64_t w1 = w0 + kSymbolSamples;
    const PathDelays paths = s1_path_delays(w1, cfo, chk.mode_id);
    // 路徑的 S1 有效部分從 w1 + 延遲 開始，往前推一個循環字首和一個符號就是 S0 的起點
    auto frame_start = [&](double delay) { return w1 + std::llround(std::clamp(delay, -64.0, 96.0)) - kCyclicPrefix - kSymbolSamples; };
    double tau = paths.first;
    std::int64_t t0 = frame_start(tau);
    if (t0 + kCyclicPrefix - kTrackBackoff < base_) return false;

    // 第二道驗證：酬載開頭的導頻要與 S1 估出的通道相符。雜訊偶爾會通過前導符號的檢查，
    // 但不會連導頻都對得上；有了這一步，前面的門檻可以放低，弱訊號比較不會漏掉。
    PilotCheck pilots = verify_pilots(t0, cfo, *mode);
    if (std::fabs(paths.first - paths.peak) > 8.0) {
        // 「第一條路徑」可能只是雜訊：以最強路徑為準再驗證一次，取相關較高的定時
        const std::int64_t alt = frame_start(paths.peak);
        if (alt + kCyclicPrefix - kTrackBackoff >= base_) {
            const PilotCheck other = verify_pilots(alt, cfo, *mode);
            if (other.rho > pilots.rho) {
                pilots = other;
                tau = paths.peak;
                t0 = alt;
            }
        }
    }
    if (trace_) {
        std::fprintf(stderr, "[rx] %s：模式 %d、頻偏 %+.2f Hz（導頻修正 %+.2f）、S1 第一條路徑 %+.1f、幀起點 %lld、導頻相關 %.2f\n",
                     pilots.rho >= tuning_.verify_pilots ? "捕獲" : "導頻不符，放棄", mode->id, cfo * kSampleRate,
                     pilots.cfo_delta * kSampleRate, tau, static_cast<long long>(t0), pilots.rho);
    }
    if (pilots.rho < tuning_.verify_pilots) return false;
    if (receiving && !tentative()) {
        // 目前這一幀照樣收完（少了後面一截也可能解得出來），之後從這個前導符號重新開始
        if (trace_) std::fprintf(stderr, "[rx]   目前這一幀還沒收完就出現另一個前導符號：收完後改從 %lld 開始\n", static_cast<long long>(t0));
        pending_start_ = t0;
        scan_limit_ = scan_pos_;  // 不必再找了
        return false;
    }
    cfo += pilots.cfo_delta;

    mode_ = mode;
    cfo_ = cfo;
    t0_ = t0;
    state_ = State::Receive;
    confirmed_ = true;
    presence_checked_ = true;
    rescan_from_ = t0 + kSymbolSamples;  // 這一幀若沒解出來、也沒有下一幀，就從這個前導符號之後重新搜尋
    unconfirmed_run_ = 0;
    last_ok_ = false;
    last_complete_ = false;
    sync_quality_ = chk.score;
    frame_evidence_ = chk.score;
    if (tentative()) {
        // 證據薄弱的捕獲有可能是誤認，而真正的前導符號常常就在附近（弱訊號時候選位置可以差一百多個取樣，
        // S0 又每 128 點重複）：接著原來的位置繼續找，不要跳到這一幀的前導符號之後，免得把它跳過去。
        pending_start_ = -1;
        scan_limit_ = t0 + mode->frame_samples() - kSymbolSamples;
    } else {
        start_scan(rescan_from_, t0 + mode->frame_samples() - kSymbolSamples);
    }
    analog_gain_code_ = -1;
    // 解出第一幀之前，先用 S0 相關值粗估訊雜比：sqrt(metric) ≈ S/(S+N)
    const double r = std::min(std::sqrt(metric), 0.999);
    snr_db_ = static_cast<float>(10.0 * std::log10(r / (1.0 - r)));
    snr_valid_ = true;
    return true;
}

SymbolGrid Receiver::demodulate(std::int64_t t0, double cfo, int rows) const {
    SymbolGrid grid(rows);
    // 視窗提前 kTrackBackoff 個取樣，會讓第 k 格多出 exp(−j2πk·提前量/N) 的相位，這裡乘回去
    std::array<cf, kCarriers> derotate;
    for (int c = 0; c < kCarriers; ++c) {
        derotate[static_cast<std::size_t>(c)] = std::polar(1.0f, static_cast<float>(kTwoPi * (kFirstBin + c) * kTrackBackoff / kFftSize));
    }
    cf y[kFftSize];
    for (int r = 0; r < rows; ++r) {
        fft_window(t0 + static_cast<std::int64_t>(r) * kSymbolSamples + kCyclicPrefix - kTrackBackoff, cfo, y);
        for (int c = 0; c < kCarriers; ++c) grid.at(r, c) = y[kFirstBin + c] * derotate[static_cast<std::size_t>(c)];
    }
    return grid;
}

// 捕獲後的驗證：以 S1 的通道估計（在延遲域只留通道可能出現的範圍）預測酬載開頭幾個符號的導頻。
// 每個符號先把導頻相干累加，得到一個複數；殘餘頻偏會讓它逐符號旋轉，所以在 ±16 Hz 內搜尋
// 讓各符號同相的頻率，回傳那時的正規化相關與頻偏。
// 這同時解決了一個問題：S0 的頻偏估計在弱訊號下誤差可達數 Hz，而整幀導頻的頻偏估計
// 只能分辨 ±6.25 Hz（導頻每 3 個符號才回到同一格），差太多會鎖到錯的那一圈。
Receiver::PilotCheck Receiver::verify_pilots(std::int64_t t0, double cfo, const Mode& mode) const {
    const SymbolGrid grid = demodulate(t0, cfo, kPreambleSymbols + kVerifySymbols);
    cf x[kFftSize];
    std::fill(x, x + kFftSize, cf(0, 0));
    const float s1_level = clip_level(&grid.at(1, 0), kCarriers);
    for (int c = 0; c < kCarriers; ++c) x[kFirstBin + c] = limited(grid.at(1, c), s1_level) / s1_value(mode.id, kFirstBin + c);
    fft256().inverse(x);
    for (int t = 0; t < kFftSize; ++t) {
        const int delay = t < kFftSize / 2 ? t : t - kFftSize;
        x[t] = (delay >= -kChanestDelayBefore && delay <= kChanestDelayAfter) ? x[t] * (1.0f / kFftSize) : cf(0, 0);
    }
    fft256().forward(x);

    std::array<cd, kVerifySymbols> acc{};
    double ey = 0.0, ep = 0.0;
    for (int j = 0; j < kVerifySymbols; ++j) {
        const float level = clip_level(&grid.at(j + kPreambleSymbols, 0), kCarriers);
        for (int c = 0; c < kCarriers; ++c) {
            const int bin = kFirstBin + c;
            if (!is_pilot(j, bin)) continue;
            const cf y = limited(grid.at(j + kPreambleSymbols, c), level);
            const cf predicted = x[bin] * pilot_value(bin);
            acc[static_cast<std::size_t>(j)] += cd(y * std::conj(predicted));
            ey += std::norm(y);
            ep += std::norm(predicted);
        }
    }
    // 以全部 12 個符號的能量正規化（而不是逐符號）：其中幾個符號如果根本沒有訊號，相關值就會跟著變低。
    // 真正的幀在前導符號之後一定有這麼多個酬載符號。
    const double norm = std::sqrt(ey * ep);
    if (norm <= 0.0) return {};

    // 第 j 個酬載符號距離 S1 是 j+1 個符號
    auto magnitude = [&](double hz) {
        cd sum(0, 0);
        for (int j = 0; j < kVerifySymbols; ++j) {
            sum += acc[static_cast<std::size_t>(j)] * std::polar(1.0, -kTwoPi * hz * (j + 1) * kSymbolSamples / kSampleRate);
        }
        return std::abs(sum);
    };
    constexpr double kStepHz = 0.5, kRangeHz = 16.0;
    double best_hz = 0.0, best = -1.0;
    for (double hz = -kRangeHz; hz <= kRangeHz; hz += kStepHz) {
        const double m = magnitude(hz);
        if (m > best) {
            best = m;
            best_hz = hz;
        }
    }
    // 以拋物線內插峰值的位置
    const double left = magnitude(best_hz - kStepHz), right = magnitude(best_hz + kStepHz);
    const double curvature = left - 2.0 * best + right;
    if (curvature < 0.0) best_hz += 0.5 * kStepHz * (left - right) / curvature;
    PilotCheck out;
    out.rho = best / norm;
    out.cfo_delta = best_hz / kSampleRate;
    return out;
}

// 開頭幾十個符號裡導頻在不在（見 pilot_coherence）。
bool Receiver::pilots_present(std::int64_t t0, double cfo, const Mode& mode) const {
    const int rows = std::min(mode.frame_symbols(), kPreambleSymbols + kPresenceSymbols);
    return pilot_coherence(demodulate(t0, cfo, rows), 0, rows - kPreambleSymbols) > kPresenceThreshold;
}

void Receiver::process_frame() {
    const Mode& mode = *mode_;
    const int rows = mode.frame_symbols();
    const bool previous_ok = last_ok_;
    const std::int64_t t0_before = t0_;
    const double cfo_before = cfo_;
    SymbolGrid grid = demodulate(t0_, cfo_, rows);

    // 殘餘頻偏：由導頻估出後重新解調（頻偏會造成載波間干擾，不能只在頻域補相位）
    for (int pass = 0; pass < 2; ++pass) {
        const double dphi = chanest_.residual_phase_per_symbol(mode, grid);
        const double dcfo = dphi / (kTwoPi * kSymbolSamples);
        if (std::fabs(dcfo * kSampleRate) < kCfoRedoHz) break;
        cfo_ += dcfo;
        grid = demodulate(t0_, cfo_, rows);
    }

    // 定時：整幀導頻平均出來的第一條路徑若偏離基準，就移動視窗重新解調
    const double tau = std::clamp(chanest_.first_path_delay(mode, grid), -48.0, 48.0);
    if (std::fabs(tau) >= 3.0) {
        const std::int64_t moved = t0_ + std::llround(tau);
        if (moved + kCyclicPrefix - kTrackBackoff >= base_ && moved + mode.frame_samples() + kCyclicPrefix <= end()) {
            t0_ = moved;
            grid = demodulate(t0_, cfo_, rows);
        }
    }

    const ChannelEstimate est = chanest_.estimate(mode, grid);
    snr_db_ = est.snr_db;
    snr_valid_ = true;
    // 整幀的導頻在不在。前導符號偶爾會被雜訊（或別的訊號）騙過，這時解出來的只是雜訊，不能當成一幀送出去。
    const double coherence = pilot_coherence(grid, 0, mode.payload_symbols);
    const bool present = coherence > kPresenceThreshold;
    // 把酬載分成 2–4 段，每一段都有導頻：對方把整幀送完了（不是送到一半就停，也不是後面一截換成別的訊號）
    bool present_throughout = true;
    const int segments = std::clamp(mode.payload_symbols / 18, 2, 4);
    for (int k = 0; k < segments && present_throughout; ++k) {
        const int from = mode.payload_symbols * k / segments, to = mode.payload_symbols * (k + 1) / segments;
        present_throughout = pilot_coherence(grid, from, to) > kPresenceThreshold;
    }
    // 幀尾附近如果有很像 S0 的東西還來不及判定（要等後面十幾個符號到齊），這一幀可能在那裡就被別的發射蓋掉了
    const bool late_candidate = (cand_ready_ && cand_metric_ >= kSolidScThreshold) || (peak_active_ && peak_metric_ >= kSolidScThreshold);
    const bool intact = present_throughout && pending_start_ < 0 && !late_candidate;
    if (trace_) {
        std::fprintf(stderr, "[rx] 幀 @%lld 模式 %d：頻偏 %+.2f Hz、定時修正 %+.1f、訊雜比 %.1f dB、導頻相干 %.2f\n",
                     static_cast<long long>(t0_), mode.id, cfo_ * kSampleRate, tau, est.snr_db, coherence);
    }

    // 等化資料格
    const std::vector<CellPos> positions = data_cell_positions(mode.payload_symbols);
    std::vector<cf> cells(positions.size());
    std::vector<float> noise(positions.size());
    for (std::size_t n = 0; n < positions.size(); ++n) {
        const int row = positions[n].symbol + kPreambleSymbols;
        const int c = positions[n].bin - kFirstBin;
        const cf h = est.at(row, c);
        const float h2 = std::max(std::norm(h), 1e-20f);
        cells[n] = grid.at(row, c) * std::conj(h) / h2;
        noise[n] = (tuning_.noise_map ? est.noise_at(row, c) : est.noise_var) * (1.0f + est.gain_at(row, c)) / h2;
    }

    RxFrame frame;
    frame.mode_id = mode.id;
    frame.is_digital = mode.is_digital();
    frame.snr_db = est.snr_db;
    frame.freq_offset_hz = static_cast<float>(cfo_ * kSampleRate);
    frame.start_sample = t0_;

    bool ok = false;
    bool emit = false;
    if (mode.is_digital()) {
        DigitalDecoded d = digital_decode(mode, cells, noise);
        if (!d.crc_ok) {
            // S1 可能把碼率認錯：同樣調變、同樣幀長的其他模式也試一次
            for (const Mode& other : kModes) {
                if (&other == &mode || !other.is_digital()) continue;
                if (other.bits_per_cell != mode.bits_per_cell || other.payload_symbols != mode.payload_symbols) continue;
                DigitalDecoded alt = digital_decode(other, cells, noise);
                if (alt.crc_ok) {
                    d = std::move(alt);
                    mode_ = &other;
                    frame.mode_id = other.id;
                    break;
                }
            }
        }
        ok = d.crc_ok;
        frame.decoded = ok;
        frame.header_ok = ok;
        frame.ldpc_iterations = d.ldpc_iterations;
        if (ok) {
            frame.callsign = d.header.callsign_valid ? d.header.callsign : std::string();
            frame.sequence = d.header.sequence;
            frame.image_format = d.header.image_format;
            frame.image_data = std::move(d.image);
        }
        // 沒解出來的幀：前導符號確認過、從頭到尾都有導頻、中途也沒有出現別的前導符號，才回報「有一幀沒解出來」。
        // 只有開頭一段有導頻的，是對方送到一半就停了，或是誤認的前導符號剛好落在別人的酬載裡。
        emit = ok || (confirmed_ && intact);
    } else {
        AnalogDecoded a = analog_decode(mode, cells, noise, analog_gain_code_);
        ok = a.header_ok;
        frame.header_ok = a.header_ok;
        if (a.header_ok) {
            analog_gain_code_ = a.header.gain_code;
            frame.callsign = a.header.callsign_valid ? a.header.callsign : std::string();
            frame.sequence = a.header.sequence;
        }
        // 畫面本身不靠標頭，標頭壞了也還原得出來（幀增益沿用上一張的，或用名目值）。但只有確定這是一整幀
        // 才當成畫面：前導符號很明確或緊接在一張好的後面、從頭到尾都有導頻、中途也沒有出現別的前導符號。
        // 否則多半只是雜訊、送到一半就停的幀，或是對到別的訊號上；這時只回報「有一幀沒解出來」。
        const bool picture = a.header_ok || (confirmed_ && (previous_ok || !tentative()) && intact);
        emit = picture || (confirmed_ && intact);
        frame.decoded = picture;
        if (picture) frame.rgb = std::move(a.rgb);
    }

    if (trace_) std::fprintf(stderr, "[rx]   → %s（LDPC %d 次疊代）\n", ok ? "解碼成功" : "解碼失敗", frame.ldpc_iterations);
    if (emit) {
        if (frame.decoded) ++frames_decoded_;
        else ++frames_failed_;
        frames_.push_back(std::move(frame));
    }
    last_ok_ = ok;
    last_complete_ = false;
    if (!ok && !present) {
        // 整幀都沒有導頻：這一幀估出來的頻偏與定時只是雜訊，不採用
        t0_ = t0_before;
        cfo_ = cfo_before;
    }

    // 下一幀緊接在後
    const std::int64_t frame_start = t0_;
    t0_ += mode_->frame_samples();
    if (ok) {
        // 解出來的這一幀不必再搜尋。不過解得出來不代表對方把整幀送完了：類比的標頭、低碼率的數位模式
        // 都禁得起少掉後面一截。從尾端一段一段往回看，導頻在哪裡消失，就從那裡開始算「還沒看過」，
        // 另一次發射可能就在那之後開始。
        int seen = mode.payload_symbols;  // 導頻一直到第幾個酬載符號都還在
        while (seen > 0) {
            const int first = seen >= 2 * kTailSegment ? seen - kTailSegment : 0;
            if (pilot_coherence(grid, first, seen) > kPresenceThreshold) break;
            seen = first;
        }
        // 就算導頻一直都在，最後十幾個符號也再看一次：一邊收一邊找 S0 的時候，候選位置要等後面十幾個符號
        // 到齊才能判定，落在幀尾的來不及。
        const int unseen_tail = kPreambleSymbols + kVerifySymbols + 2;
        const int rescan_row = std::min(seen >= mode.payload_symbols ? mode.payload_symbols : std::max(seen - kTailSegment, 0),
                                        std::max(mode.payload_symbols - unseen_tail, 0));
        rescan_from_ = frame_start + static_cast<std::int64_t>(kPreambleSymbols + rescan_row) * kSymbolSamples;
        last_complete_ = seen >= mode.payload_symbols;
        if (trace_ && !last_complete_) {
            std::fprintf(stderr, "[rx]   導頻只到第 %d 個酬載符號（共 %d 個）：對方可能中途停了\n", seen, mode.payload_symbols);
        }
    }
    if (pending_start_ >= 0) {
        // 這一幀收到一半時，裡面出現了另一個很明確的前導符號：直接去收那一次發射
        const std::int64_t from = pending_start_ - kSymbolSamples;
        mode_ = nullptr;
        enter_search(from);
        return;
    }
    state_ = State::Confirm;
}

void Receiver::confirm_next() {
    static const std::array<int, 1> no_shift = {0};
    // 追蹤時定時已知：第一條路徑在視窗起點之後 kTrackBackoff 個取樣，其餘路徑在它之後 4 ms 內。
    // 延遲的範圍取得比捕獲時窄；位置差太多的前導符號不是這次發射的下一幀，留給搜尋去處理。
    const PreambleCheck chk = check_preamble(t0_ + kCyclicPrefix - kTrackBackoff, cfo_, no_shift, mode_->id, kTrackDetectFirst, kTrackDetectLast);
    sync_quality_ = std::max(chk.score_mode, chk.score);
    if (trace_) {
        std::fprintf(stderr, "[rx] 下一幀 @%lld 的前導符號：目前模式分數 %.2f，最佳模式 %d 分數 %.2f（S1 %.2f）\n",
                     static_cast<long long>(t0_), chk.score_mode, chk.mode_id, chk.score, chk.score_s1);
    }

    // 上一幀解出來了、導頻也一直到幀尾都在，對方多半還在送，門檻可以低；上一幀沒解出來，或是後面一截
    // 沒有導頻（對方中途停了），就要和捕獲時一樣確定才算數——否則在別人的酬載或雜訊裡偶爾超過低門檻，
    // 就會白收一整幀，還可能錯過這段時間裡開始的另一次發射。
    bool confirmed = chk.score_mode >= (last_complete_ ? tuning_.track_score : tuning_.acquire_score);
    // 換模式：別的模式的分數明顯比目前的模式高。上一幀沒解出來（或沒送完）的話，要非常確定才算數。
    const double switch_score = last_complete_ ? tuning_.switch_score : 2.0 * tuning_.acquire_score;
    const Mode* const previous_mode = mode_;
    if (chk.mode_id != mode_->id && chk.score >= switch_score && chk.score > 1.3 * chk.score_mode) {
        // 對方換了模式
        mode_ = find_mode(chk.mode_id);
        confirmed = true;
    }

    if (confirmed && !last_complete_) {
        // 上一幀沒解出來（或沒送完）：頻偏與定時都不那麼可靠了，前導符號的分數也可能是別的訊號湊巧造成的。
        // 和捕獲時一樣，再用酬載開頭的導頻驗證一次，順便把頻偏校正回來。
        const PilotCheck pilots = verify_pilots(t0_, cfo_, *mode_);
        if (trace_) std::fprintf(stderr, "[rx]   上一幀不完整，導頻驗證：相關 %.2f、頻偏修正 %+.2f Hz\n", pilots.rho, pilots.cfo_delta * kSampleRate);
        if (pilots.rho >= tuning_.verify_pilots) {
            cfo_ += pilots.cfo_delta;
        } else {
            confirmed = false;
            mode_ = previous_mode;
        }
    }

    if (confirmed) {
        if (mode_ != previous_mode) analog_gain_code_ = -1;
        confirmed_ = true;
        presence_checked_ = true;
        unconfirmed_run_ = 0;
        rescan_from_ = t0_ + kSymbolSamples;  // 前一幀就算沒解出來，也確實是一幀：不必回頭搜尋它
        frame_evidence_ = std::max(chk.score_mode, chk.mode_id == mode_->id ? chk.score : 0.0);
        state_ = State::Receive;
        start_scan(rescan_from_, t0_ + mode_->frame_samples() - kSymbolSamples);
        return;
    }
    // 沒看到前導符號：上一幀是好的就再撐一幀（可能只是剛好衰落），否則回去搜尋
    if (last_ok_ && unconfirmed_run_ == 0) {
        ++unconfirmed_run_;
        confirmed_ = false;
        presence_checked_ = false;
        frame_evidence_ = 0.0;
        state_ = State::Receive;
        start_scan(rescan_from_, t0_ + mode_->frame_samples() - kSymbolSamples);
        return;
    }
    mode_ = nullptr;
    // 上一幀沒解出來、後面也沒有前導符號：對方可能在那一幀的中途就停了（或者那一幀根本是誤判），
    // 另一次發射可能就在那段時間裡開始，所以從 rescan_from_ 重新搜尋，而不是從這裡。
    enter_search(rescan_from_);
}

}  // namespace nbtv
