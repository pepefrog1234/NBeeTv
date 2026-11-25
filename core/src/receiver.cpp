#include "receiver.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>

#include "fft.hpp"
#include "frame_map.hpp"
#include "learned_frame.hpp"

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
// 照節拍接著收（RxTuning::follow）：前導符號沒確認到、標頭也沒解出來的幀，要整幀的導頻相干值超過這個門檻、
// S1 也和目前的模式對得上，才當成一幀。導頻相干值在有訊號時接近導頻格的 S/(S+N)（3 kHz 訊雜比 −6 dB 約 0.36、
// −4 dB 約 0.47），只有雜訊時約 0.08（最短的模式 14；幀愈長愈小），比「導頻在不在」的門檻嚴。
// 這個門檻要過兩次：全部的導頻一次，不算能量最強的 3 個載波再一次（見 process_frame；後者擋的是強度中等的單頻載波）。
constexpr double kFollowThreshold = 0.3;
// 導頻的能量集中在少數幾個載波上（見 pilot_spread）：頻帶內有一個強的單頻載波。載波在相隔幾個符號之間本來就是相干的，
// 漏到鄰近幾格的部分也是，光看導頻的相干值和導頻分不出來：發射結束後只剩雜訊與一個 1200 Hz 的載波時，相干值有 0.53，
// 最強的 3 個載波佔了 83%。真正的訊號能量分散在整個頻帶，最強的 3 個載波在白雜訊下佔 13–24%、
// 衰落通道（差通道、中等、慢衰落）最多 29%。超過這個比例時，標頭沒解出來的幀不能只憑導頻就算數：
// 靠前導符號的要另外核對 S1（kCarrierS1），照節拍接著收的不接。
constexpr double kPilotTopShare = 0.5;
// S1 的核對（見 s1_agreement）：目前模式的相關值至少要這麼高，而且別的模式不能比它高出 kFollowS1Margin 以上。
// 有 S1 而且是目前的模式時，白雜訊下 3 kHz 訊雜比 −6 dB 平均 0.42、−4 dB 0.52、0 dB 0.73（標準差 0.07–0.12；
// 差通道 0.17–0.18），這時「其餘最高的」約是它的四成（序列在奇數載波上最多有四成相像）。
// 那裡不是 S1（酬載、雜訊）時在 0 附近、標準差約 0.15（只用 21 個奇數載波）。
// 模擬裡真正的幀通過的比例：白雜訊 −4 dB 以上 99–100%、−6 dB 是 94%；差通道 −6 到 0 dB 是 89–98%。
constexpr double kFollowS1 = 0.2;
constexpr double kFollowS1Margin = 0.1;
// 導頻的能量集中（有強的載波）時，靠前導符號確認的幀要 S1 的相關值到這麼高才算數：真的幀就算疊著比它強的載波，
// 0 dB 以上也有 0.6–0.7；只有載波與雜訊時在 0 附近（標準差約 0.15），超過的機會約 1%。
constexpr double kCarrierS1 = 0.35;
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
// 判定前導符號與導頻在不在的統計量，都假設能量大致平均分布在各個載波上。如果有幾個格點比其他的大上幾十倍
// （窄頻的干擾，或是能量集中在少數幾個載波上的別種訊號），少數幾項就足以左右整個統計量，
// 於是在不是前導符號的地方誤判出前導符號或導頻。把過大的值壓到上限（只改大小、不改相位）之後就不會；
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
// 這個比例也只有五成左右；只有導頻那八九個載波有能量的符號（資料格幾乎是 0），比例會超過九成。
// 那種符號每 128 點重複（導頻每 6 格一個），S0 的滑動相關會把它當成候選，導頻的 PRBS 又可能湊巧和某個模式的
// S1 序列有幾分相似，所以要另外擋掉。現在的模式資料格都是滿功率，自己的訊號不會出現這種符號；
// 這一關擋的是別種只佔幾個載波的訊號。
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

// 酬載符號裡導頻的幅度上限：只看「這個符號的導頻位置」上那八九個格點，取它們幅度中位數的 kLimitFactor 倍。
// row 是格點陣列的列號，j 是它在酬載裡的符號編號（決定導頻在哪幾格）。
//
// 這裡不取整個符號 49 個載波的中位數：資料格的幅度隨畫面的內容而變，如果大半的資料格接近 0，
// 整個符號的中位數就只剩雜訊的大小，導頻全被壓到那麼小，真正的幀反而通不過驗證（訊號愈乾淨愈容易發生）。
// 只看導頻位置的話，真正的幀在這些格點上都是導頻，中位數就是導頻的幅度，和資料格放了什麼無關；
// 不是幀的地方，這些格點上少數特別大的值照樣會被壓下來。
float pilot_clip_level(const SymbolGrid& grid, int row, int j) {
    std::array<float, kCarriers> mag;
    int count = 0;
    for (int c = 0; c < kCarriers; ++c) {
        if (is_pilot(j, kFirstBin + c)) mag[static_cast<std::size_t>(count++)] = std::abs(grid.at(row, c));
    }
    if (count == 0) return 0.0f;
    std::nth_element(mag.begin(), mag.begin() + count / 2, mag.begin() + count);
    return kLimitFactor * mag[static_cast<std::size_t>(count / 2)];
}

// 同一格相鄰兩個導頻（相隔 3 個符號）是否相干：有訊號而且位置對的時候接近 S/(S+N)，
// 只有雜訊時接近 0（約 1/sqrt(導頻對數)）。只看第 first 到 last−1 個酬載符號。
// 每一對符號（第 j 個與第 j+3 個）先各自算出正規化的相關值，再對所有的符號對取平均：每一對的份量相同，
// 不會被少數幾個特別強的符號左右（例如檢查的範圍裡大半是雜訊、最後幾個符號卻是另一次發射的開頭），
// 完全沒有訊號的符號（取樣全是 0 的靜音）也照樣算一對、相關值是 0。
// skip 不是空指標時，那幾個載波不算（見 PilotSpread：排除能量最強的幾個載波之後仍然相干，才不是單頻載波造成的）。
double pilot_coherence(const SymbolGrid& grid, int first, int last, const std::array<int, 3>* skip = nullptr) {
    cd total(0, 0);
    int pairs = 0;
    for (int j = first; j + 3 < last; ++j) {
        const int ra = j + kPreambleSymbols, rb = ra + 3;
        const float la = pilot_clip_level(grid, ra, j), lb = pilot_clip_level(grid, rb, j);
        cd acc(0, 0);
        double ea = 0.0, eb = 0.0;
        for (int c = 0; c < kCarriers; ++c) {
            if (!is_pilot(j, kFirstBin + c)) continue;  // 第 j 與第 j+3 個符號的導頻在同樣的格子上
            if (skip != nullptr && (c == (*skip)[0] || c == (*skip)[1] || c == (*skip)[2])) continue;
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

// 導頻的能量有多集中：25 個有導頻的載波（偶數格）各自把整幀的導頻能量加起來，找出最強的 3 個。
struct PilotSpread {
    double top_share = 1.0;                // 最強的 3 個載波佔全部導頻能量的比例
    std::array<int, 3> top = {-1, -1, -1};  // 它們的載波編號（0..48）
};

PilotSpread pilot_spread(const SymbolGrid& grid, int payload_symbols) {
    constexpr int kEvenCarriers = (kCarriers + 1) / 2;
    std::array<double, kEvenCarriers> energy{};
    double total = 0.0;
    for (int j = 0; j < payload_symbols; ++j) {
        for (int c = 0; c < kCarriers; c += 2) {
            if (!is_pilot(j, kFirstBin + c)) continue;
            const double e = std::norm(grid.at(j + kPreambleSymbols, c));
            energy[static_cast<std::size_t>(c / 2)] += e;
            total += e;
        }
    }
    PilotSpread out;
    if (total <= 0.0) return out;
    double sum = 0.0;
    for (int n = 0; n < 3; ++n) {
        const int i = static_cast<int>(std::max_element(energy.begin(), energy.end()) - energy.begin());
        sum += energy[static_cast<std::size_t>(i)];
        energy[static_cast<std::size_t>(i)] = -1.0;
        out.top[static_cast<std::size_t>(n)] = 2 * i;
    }
    out.top_share = sum / total;
    return out;
}

// 這一幀的 S1 是不是目前這個模式的：用整幀估出的通道把 S1 那一列和 16 條 S1 序列逐一做相干相關。
// own 是目前模式的正規化相關值（訊號乾淨時接近 1；那裡沒有 S1、或只有雜訊時在 0 附近），
// other 是其餘 15 個編號裡最高的（對方換了模式，或接著來的是別種模式的幀時，它會比 own 高）。
// 只看奇數載波：偶數載波上的 S1 被通道估測當成已知值用掉了（chanest.cpp），拿來比會偏向目前的模式。
// 通道用的是緊接在 S1 後面的第一個酬載符號的估計（通道估測只算酬載的列；相隔一個符號，通道幾乎沒有變）。
// 它是整幀的導頻一起估出來的，所以這比前導符號的判定（check_preamble，只靠 S0、S1 自己）靈敏得多，
// 前導符號淹在雜訊裡時仍然分得出來。能量最強的 3 個奇數載波不算：單頻載波如果剛好落在奇數格上，導頻幾乎看不到它，
// 這裡卻整個相關值都由它決定；真正的 S1 各個載波一樣強，少算 3 個幾乎沒有差別。其餘過大的格點同樣先壓到上限（見 kLimitFactor）。
struct S1Match {
    double own = 0.0;
    double other = 0.0;
    int other_id = -1;
};

S1Match s1_agreement(const Mode& mode, const SymbolGrid& grid, const ChannelEstimate& est) {
    constexpr int kOdd = kCarriers / 2;  // 24 個
    const float level = clip_level(&grid.at(1, 1), kOdd, 2);
    std::array<float, kOdd> power;
    for (int i = 0; i < kOdd; ++i) power[static_cast<std::size_t>(i)] = std::norm(grid.at(1, 2 * i + 1));
    std::array<float, kOdd> sorted = power;
    std::nth_element(sorted.begin(), sorted.begin() + 2, sorted.end(), std::greater<float>());
    const float third = sorted[2];  // 第三強的能量：比它強或一樣強的不算（最多 3 個）
    int skipped = 0;
    std::array<double, kModeIdCount> sum{};
    double er = 0.0, eh = 0.0;
    for (int c = 1; c < kCarriers; c += 2) {
        if (skipped < 3 && power[static_cast<std::size_t>(c / 2)] >= third) {
            ++skipped;
            continue;
        }
        const cf r = limited(grid.at(1, c), level);
        const cf h = est.at(kPreambleSymbols, c);
        const double a = static_cast<double>(r.real()) * h.real() + static_cast<double>(r.imag()) * h.imag();  // Re(r·h*)
        er += std::norm(r);
        eh += std::norm(h);
        for (int id = 0; id < kModeIdCount; ++id) sum[static_cast<std::size_t>(id)] += s1_value(id, kFirstBin + c) > 0.0f ? a : -a;
    }
    S1Match match;
    if (er <= 0.0 || eh <= 0.0) return match;
    const double norm = std::sqrt(er * eh);
    match.own = sum[static_cast<std::size_t>(mode.id)] / norm;
    for (int id = 0; id < kModeIdCount; ++id) {
        if (id == mode.id) continue;
        const double v = sum[static_cast<std::size_t>(id)] / norm;
        if (match.other_id < 0 || v > match.other) {
            match.other = v;
            match.other_id = id;
        }
    }
    return match;
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
    earlier_.clear();
    frames_decoded_ = 0;
    frames_failed_ = 0;
    mode_ = nullptr;
    snr_valid_ = false;
    sync_quality_ = 0.0;
    last_ok_ = false;
    last_clear_ = false;
    follow_misses_ = 0;
    last_complete_ = false;
    unconfirmed_run_ = 0;
    rescan_from_ = 0;
    frame_evidence_ = 0.0;
    pending_start_ = -1;
    done_start_ = INT64_MIN;
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
//
// S1 的 16 條序列全部都比（kModeIdCount），不只模式表裡的那幾條：S0 是所有模式共用的，
// 別的模式的幀（已經移除的數位與類比模式，或是以後才加的模式）一樣會通過 S0 的檢查，酬載裡也一樣有導頻。
// 只比自己認得的序列的話，那種幀會被當成自己的某個模式收下來。全部都比，才看得出最像的其實是別的編號；
// 回傳的 mode_id 可能是模式表裡沒有的，由呼叫端決定不收。
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
        std::array<Hypothesis, kModeIdCount> hyp{};
        double s1_peak = 0.0;  // S1 單獨的分數在所有模式編號、所有延遲裡的最大值
        for (std::size_t mi = 0; mi < hyp.size(); ++mi) {
            const int mode_id = static_cast<int>(mi);
            std::fill(b, b + kFftSize, cf(0, 0));
            double e1 = 0.0;
            for (int c = 0; c < kCarriers; ++c) {
                const int bin = kFirstBin + c;
                const cf v = limited(y1[bin + shift], level1) * (s1_value(mode_id, bin) > 0 ? 1.0f : -1.0f);
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
        for (std::size_t mi = 0; mi < hyp.size(); ++mi) {
            const int mode_id = static_cast<int>(mi);
            if (hyp[mi].score_s1 < s1_floor || hyp[mi].score_s1 < kS1BalanceRatio * hyp[mi].score_s0) continue;
            if (hyp[mi].score > best.score) {
                best.score = hyp[mi].score;
                best.score_s1 = hyp[mi].score_s1;
                best.mode_id = mode_id;
                best.shift = shift;
            }
            if (shift == 0 && mode_id == expect_mode) best.score_mode = hyp[mi].score;
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
    // 最像的是模式表裡沒有的編號：別的模式的幀（舊版的數位、類比模式，或這一版還不認得的模式），不收
    const Mode* mode = find_mode(chk.mode_id);
    if (mode == nullptr) {
        if (trace_) std::fprintf(stderr, "[rx]   模式 %d 不在模式表裡，不收\n", chk.mode_id);
        return false;
    }
    // 正在收一幀：暫定的幀，新的前導符號要比它更確定才取代它；證據很強的幀，只理會同樣很明確的前導符號
    const bool receiving = state_ == State::Receive;
    if (receiving && (tentative() ? chk.score <= frame_evidence_ : chk.score < 2.0 * tuning_.acquire_score)) return false;

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
    // 已經收完的幀不再收一次。搜尋都從上一幀的前導符號之後開始，候選位置往回最多只看到那之前 248 個取樣，
    // 照理不會再找到同一幀；這一關是保險：不管流程怎麼走，處理過的幀起點只會愈來愈後面，
    // 所以手上的取樣一定處理得完，不會在同樣幾個位置之間來回。
    if (t0 < done_start_ + kCyclicPrefix) {
        if (trace_) std::fprintf(stderr, "[rx]   幀起點 %lld 的那一幀已經收過了，不再收一次\n", static_cast<long long>(t0));
        return false;
    }
    const Acquisition found{mode, cfo + pilots.cfo_delta, t0, chk.score, metric};
    if (receiving && !tentative()) {
        // 目前這一幀照樣收完（少了後面一截也可能解得出來），之後直接去收這個前導符號開始的那一幀
        if (trace_) std::fprintf(stderr, "[rx]   目前這一幀還沒收完就出現另一個前導符號：收完後改從 %lld 開始\n", static_cast<long long>(t0));
        pending_ = found;
        pending_start_ = t0;
        scan_limit_ = scan_pos_;  // 不必再找了
        return false;
    }
    begin_frame(found);
    return true;
}

// 開始收一幀。它的前導符號已經通過 try_acquire 的檢查。
void Receiver::begin_frame(const Acquisition& found) {
    mode_ = found.mode;
    cfo_ = found.cfo;
    t0_ = found.t0;
    state_ = State::Receive;
    confirmed_ = true;
    presence_checked_ = true;
    rescan_from_ = found.t0 + kSymbolSamples;  // 這一幀若沒解出來、也沒有下一幀，就從這個前導符號之後重新搜尋
    unconfirmed_run_ = 0;
    last_ok_ = false;
    last_clear_ = false;
    follow_misses_ = 0;
    last_complete_ = false;
    sync_quality_ = found.score;
    frame_evidence_ = found.score;
    if (tentative()) {
        // 證據薄弱的捕獲有可能是誤認，而真正的前導符號常常就在附近（弱訊號時候選位置可以差一百多個取樣，
        // S0 又每 128 點重複）：接著原來的位置繼續找，不要跳到這一幀的前導符號之後，免得把它跳過去。
        pending_start_ = -1;
        scan_limit_ = found.t0 + found.mode->frame_samples() - kSymbolSamples;
    } else {
        start_scan(rescan_from_, found.t0 + found.mode->frame_samples() - kSymbolSamples);
    }
    // 解出第一幀之前，先用 S0 相關值粗估訊雜比：sqrt(metric) ≈ S/(S+N)
    const double r = std::min(std::sqrt(found.metric), 0.999);
    snr_db_ = static_cast<float>(10.0 * std::log10(r / (1.0 - r)));
    snr_valid_ = true;
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
        const float level = pilot_clip_level(grid, j + kPreambleSymbols, j);
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

void Receiver::set_merge(std::shared_ptr<const LearnedMerge> merge) {
    earlier_.clear();
    if (merge == nullptr) {
        merges_.clear();
        return;
    }
    for (auto& have : merges_) {
        if (have->model_id() == merge->model_id()) {
            have = std::move(merge);
            return;
        }
    }
    merges_.push_back(std::move(merge));
}

const LearnedMerge* Receiver::find_merge(std::uint32_t model_id) const {
    for (const auto& merge : merges_)
        if (merge->model_id() == model_id) return merge.get();
    return nullptr;
}

int Receiver::merge_earlier(const LearnedMerge& merge, const Mode& mode, const LearnedUnpacked& frame, const RxFrame& info,
                            LearnedUnpacked& merged) const {
    if (frame.values.size() != static_cast<std::size_t>(merge.value_count())) return 0;
    const int slots = merge.frames() - 1;
    std::vector<LearnedMerge::Earlier> earlier(static_cast<std::size_t>(slots));
    const double frame_samples = static_cast<double>(mode.frame_samples());
    int used = 0;
    for (const Earlier& e : earlier_) {
        if (e.mode_id != mode.id || e.model_id != merge.model_id() || e.values.size() != frame.values.size()) continue;
        // 是幾幀之前的：由幀開頭的位置算。同一次發射的幀一個接一個，定時修正頂多差幾十個取樣；
        // 對不上整數幀的是另一次發射。
        const double apart = static_cast<double>(info.start_sample - e.start) / frame_samples;
        const long back = std::lround(apart);
        if (back < 1 || back > slots || std::fabs(apart - static_cast<double>(back)) > 0.2) continue;
        // 兩邊的標頭都解出來時再核對：呼號相同、幀序號剛好差這麼多
        if (e.header_ok && frame.header_ok && (e.callsign != info.callsign || ((e.sequence + back) & 0xFF) != info.sequence)) continue;
        LearnedMerge::Earlier& slot = earlier[static_cast<std::size_t>(back - 1)];
        if (!slot.values.empty()) continue;
        slot.values = e.values;
        slot.noise_var = e.noise_var;
        ++used;
    }
    if (used == 0) return 0;
    merged = frame;
    return merge.merge(frame.values, frame.noise_var, earlier, merged.values, merged.noise_var) ? used : 0;
}

void Receiver::remember(const LearnedMerge& merge, const Mode& mode, const LearnedUnpacked& frame, const RxFrame& info) {
    Earlier e;
    e.mode_id = mode.id;
    e.model_id = merge.model_id();
    e.start = info.start_sample;
    e.header_ok = frame.header_ok;
    e.sequence = info.sequence;
    e.callsign = info.callsign;
    e.values = frame.values;
    e.noise_var = frame.noise_var;
    earlier_.push_front(std::move(e));
    while (earlier_.size() > static_cast<std::size_t>(merge.frames() - 1)) earlier_.pop_back();
}

void Receiver::process_frame() {
    const Mode& mode = *mode_;
    const int rows = mode.frame_symbols();
    const bool previous_ok = last_ok_;
    const bool previous_clear = last_clear_;
    const std::int64_t t0_before = t0_;
    const double cfo_before = cfo_;
    // 前導符號確認過的幀處理一次就夠了（見 done_start_）。沒確認到前導符號、只是假設對方還在送的幀不算：
    // 那裡不一定真的有一幀，之後回頭搜尋時可能在它前後找到真正的前導符號。
    if (confirmed_) done_start_ = t0_before;
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
    // 導頻的能量是不是分散在整個頻帶：集中在少數幾個載波上的是單頻載波，不是導頻（見 kPilotTopShare）
    const PilotSpread pilots = (intact || trace_) ? pilot_spread(grid, mode.payload_symbols) : PilotSpread{};
    const bool spread = pilots.top_share < kPilotTopShare;
    // 導頻從頭到尾都很清楚：這裡確實有一整幀。門檻比「導頻在不在」高，只有雜訊時幾乎不可能超過。
    // 相干值不算能量最強的那 3 個載波：強度中等的單頻載波佔不到一半的能量，卻足以把整體的相干值拉過門檻。
    const bool follow_candidate = tuning_.follow && intact && spread && coherence > kFollowThreshold;
    const double coherence_rest = (follow_candidate || trace_) ? pilot_coherence(grid, 0, mode.payload_symbols, &pilots.top) : 0.0;
    const bool clear = follow_candidate && coherence_rest > kFollowThreshold;
    if (trace_) {
        std::fprintf(stderr, "[rx] 幀 @%lld 模式 %d：頻偏 %+.2f Hz、定時修正 %+.1f、訊雜比 %.1f dB、導頻相干 %.2f"
                             "（最強的 3 個載波佔 %.0f%%，不算它們是 %.2f）\n",
                     static_cast<long long>(t0_), mode.id, cfo_ * kSampleRate, tau, est.snr_db, coherence, 100.0 * pilots.top_share, coherence_rest);
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
    frame.snr_db = est.snr_db;
    frame.freq_offset_hz = static_cast<float>(cfo_ * kSampleRate);
    frame.start_sample = t0_;

    // 標頭帶著呼號與幀序號；畫面本身不靠標頭，標頭壞了也還原得出來。但只有確定這是一整幀才當成畫面：
    // 標頭正確，或是前導符號很明確（或緊接在一張好的後面）、從頭到尾都有導頻（而且不是單頻載波冒充的）、
    // 中途也沒有出現別的前導符號；或是照幀的節拍接在一幀導頻很清楚的幀後面（見下面）。
    // 否則多半只是雜訊、送到一半就停的幀，或是對到別的訊號上；這時頂多回報「有一幀沒解出來」。
    const LearnedUnpacked l = learned_unpack(mode, cells, noise);
    const bool ok = l.header_ok;
    frame.header_ok = l.header_ok;
    if (l.header_ok) {
        frame.callsign = l.header.callsign_valid ? l.header.callsign : std::string();
        frame.sequence = l.header.sequence;
    }
    // 靠前導符號算數的幀：前導符號確認過、而且很明確或緊接在一張好的後面，從頭到尾都有導頻。
    const bool by_preamble = !l.header_ok && confirmed_ && (previous_ok || !tentative()) && intact;
    // 照節拍接著收的候選（RxTuning::follow）：訊號弱時最先撐不住的是標頭與前導符號（3 kHz 訊雜比 −4 dB 時標頭只剩 4% 解得出來），
    // 畫面其實還解得出來。上一幀的導頻從頭到尾都很清楚、這一幀照幀長接在它後面而且導頻同樣清楚，那麼對方就是還在送。
    const bool chained = !l.header_ok && !(by_preamble && spread) && previous_clear && clear;
    // 這兩種情形要再用整幀估出的通道核對 S1 確實是目前這個模式的：
    //   - 導頻的能量集中在少數幾個載波上（有強的單頻載波）而靠前導符號的幀。只有載波與雜訊的地方，前導符號的分數與導頻的
    //     相干值都可能被載波騙過（發射結束後多交出一張不存在的畫面）；真的幀就算疊著載波，S1 仍然對得上。門檻取得比較高。
    //   - 照節拍接著收的幀。對方換了模式、後面緊接著別種模式的幀、或這裡根本不是一幀的開頭時對不上。
    //     S1 沒對上的那一幀不交出去，但只有一次的話先不放棄，下一幀再看（見下面 last_clear_ 的更新）。
    const bool suspect = by_preamble && !spread;
    const S1Match s1 = (chained || suspect || trace_) ? s1_agreement(mode, grid, est) : S1Match{};
    const bool s1_margin = s1.other - s1.own <= kFollowS1Margin;
    const bool followed = chained && s1.own >= kFollowS1 && s1_margin;
    const bool usual = l.header_ok || (by_preamble && (spread || (s1.own >= kCarrierS1 && s1_margin)));
    if (trace_) {
        std::fprintf(stderr, "[rx]   S1 的核對：目前的模式 %.2f，其餘最高的是模式 %d 的 %.2f%s\n", s1.own, s1.other_id, s1.other,
                     chained ? (followed ? "；照節拍接著收，算一幀" : "；照節拍接著收，但 S1 對不上，不算")
                             : suspect ? (usual ? "；有強的載波，S1 對得上，算一幀" : "；有強的載波，S1 對不上，不算") : "");
    }
    const bool whole = usual || followed;
    // 沒有這個模式用的模型，或標頭說對方用的是另一份權重：解出來只會是雜訊，不交出畫面，只回報收到了一幀
    const LearnedCodec* model = models_.find(mode);
    const bool usable = model != nullptr && learned_model_matches(l, *model);
    frame.model_missing = whole && !usable;
    frame.decoded = whole && usable;
    if (frame.decoded) {
        frame.rgb.resize(static_cast<std::size_t>(kImageRgbBytes));
        LearnedRxOptions options;
        options.median_snr = tuning_.learned_median_snr;
        options.attenuate = tuning_.learned_attenuate;
        options.floor = tuning_.learned_floor;
        // 合併最近幾幀（要打開、而且這個模型有合併用的權重）：畫面沒變的地方把前面幾幀收到的值合進來。
        // 記下來的是這一幀原來收到的值，不是合併後的：每一幀都只和實際收到的東西合。
        const LearnedMerge* merge = tuning_.learned_merge ? find_merge(model->model_id()) : nullptr;
        LearnedUnpacked merged;
        const int used = merge != nullptr ? merge_earlier(*merge, mode, l, frame, merged) : 0;
        const float told = learned_picture(*model, used > 0 ? merged : l, options, frame.rgb);
        if (merge != nullptr) remember(*merge, mode, l, frame);
        else earlier_.clear();
        if (trace_) std::fprintf(stderr, "[rx]   告訴解碼器的訊雜比 %.1f dB（合併了前面 %d 幀）\n", told, used);
    }
    // 沒有畫面的幀：前導符號確認過、從頭到尾都有導頻、中途也沒有出現別的前導符號，才回報「有一幀沒解出來」。
    // 只有開頭一段有導頻的，是對方送到一半就停了，或是誤認的前導符號剛好落在別人的酬載裡。
    const bool emit = whole || (confirmed_ && intact && spread);  // 有強的載波、S1 又對不上的，多半不是一幀，不回報

    if (trace_) std::fprintf(stderr, "[rx]   → 標頭%s、%s\n", ok ? "正確" : "沒解出來", frame.decoded ? "有畫面" : "沒有畫面");
    if (emit) {
        if (frame.decoded) ++frames_decoded_;
        else ++frames_failed_;
        frames_.push_back(std::move(frame));
    }
    // 交出去而且導頻很清楚的幀，就算標頭沒解出來，對追蹤來說也和解出來的一樣：對方確實送完了一整幀。
    const bool solid = whole && clear;
    last_ok_ = ok || solid;
    last_complete_ = false;
    // 下一幀能不能照節拍接著收：這一幀的導頻很清楚，而且有來歷——交出去了，或至少前導符號確認過
    // （剛捕獲、證據還不夠強所以沒交出去的第一幀也算，接下來的幀就可以由它帶出來）。
    // 接著收的幀如果只是 S1 沒對上，容許一次：可能只是 S1 那個符號剛好衰落；連續兩次就不再接。
    if (clear && (whole || confirmed_)) {
        last_clear_ = true;
        follow_misses_ = 0;
    } else if (chained && follow_misses_ == 0) {
        last_clear_ = true;
        follow_misses_ = 1;
    } else {
        last_clear_ = false;
        follow_misses_ = 0;
    }
    // 接著收而交出去的幀沒有確認過前導符號，也算處理過了：之後回頭搜尋時不要再捕獲一次
    if (solid && !confirmed_) done_start_ = std::max(done_start_, t0_before);
    if (!ok && !present) {
        // 整幀都沒有導頻：這一幀估出來的頻偏與定時只是雜訊，不採用
        t0_ = t0_before;
        cfo_ = cfo_before;
    }

    // 下一幀緊接在後
    const std::int64_t frame_start = t0_;
    t0_ += mode_->frame_samples();
    if (ok || solid) {
        // 解出來的這一幀不必再搜尋。不過標頭解得出來不代表對方把整幀送完了：它有重複與迴旋碼保護，
        // 禁得起少掉後面一截。從尾端一段一段往回看，導頻在哪裡消失，就從那裡開始算「還沒看過」，
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
        // 這一幀收到一半時，裡面出現了另一個很明確的前導符號：直接去收那一次發射。那個前導符號已經驗證過了，
        // 不回頭再搜尋一次——它可能就在這一幀開頭之後不遠（兩次發射疊在一起），回頭搜尋會先找到剛收完的
        // 這一幀的前導符號，把這一幀再收一次、又看到同一個前導符號，永遠在兩者之間來回。
        if (pending_.t0 >= done_start_ + kCyclicPrefix) {
            begin_frame(pending_);
        } else {
            mode_ = nullptr;
            enter_search(t0_before + kSymbolSamples);
        }
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
    // 前導符號的位置上像是別的模式（包括模式表裡沒有的編號）：就算沒有確定到可以換過去，
    // 也不能再假設接下來的是目前這個模式的幀，下面不照節拍接著收。
    const bool other_mode = chk.mode_id != mode_->id && chk.score >= tuning_.acquire_score && chk.score > 1.3 * chk.score_mode;
    if (chk.mode_id != mode_->id && chk.score >= switch_score && chk.score > 1.3 * chk.score_mode) {
        if (const Mode* other = find_mode(chk.mode_id)) {
            // 對方換了模式
            mode_ = other;
            confirmed = true;
        } else {
            // 接下來的是別的模式的幀（模式表裡沒有的編號）：不是我們要收的，當成沒有看到前導符號
            if (trace_) std::fprintf(stderr, "[rx]   下一幀是模式 %d，不在模式表裡，不收\n", chk.mode_id);
            confirmed = false;
        }
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
        confirmed_ = true;
        presence_checked_ = true;
        unconfirmed_run_ = 0;
        rescan_from_ = t0_ + kSymbolSamples;  // 前一幀就算沒解出來，也確實是一幀：不必回頭搜尋它
        frame_evidence_ = std::max(chk.score_mode, chk.mode_id == mode_->id ? chk.score : 0.0);
        state_ = State::Receive;
        start_scan(rescan_from_, t0_ + mode_->frame_samples() - kSymbolSamples);
        return;
    }
    // 沒看到前導符號。上一幀的導頻從頭到尾都很清楚（last_clear_）的話，對方多半還在送、只是前導符號淹在雜訊裡：
    // 照幀長繼續收，收到的那一幀要導頻同樣清楚、S1 也對得上才算數（process_frame），所以訊號一停就會停下來，
    // 不限連續幾幀。上一幀只是標頭解出來了，就只再撐一幀（可能只是剛好衰落）。否則回去搜尋。
    if (other_mode) last_clear_ = false;
    if (last_clear_ || (last_ok_ && unconfirmed_run_ == 0)) {
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
