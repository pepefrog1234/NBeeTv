#include "receiver.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
// 捕獲時幀起點最多會比候選位置晚多少：try_acquire 在候選位置前後 0、±一半、±kAcquireMaxOffset 個取樣各試一次，
// 再照 S1 量到的路徑延遲（夾在 kAcquireMinDelay…kAcquireMaxDelay）修正。驗證導頻時要讀到幀起點之後
// (kPreambleSymbols + kVerifySymbols) 個符號、扣掉 kTrackBackoff 的地方，所以 search_step 要多等 kAcquireMargin
// 個取樣才檢查候選位置。少等的話 fft_window 會讀到緩衝區尾端之外（以前只等 3 個循環字首，最多少 24 個取樣）。
constexpr int kAcquireMaxOffset = 160;
constexpr int kAcquireMinDelay = -64;
constexpr int kAcquireMaxDelay = 96;
constexpr int kAcquireMargin = 4 * kCyclicPrefix;
static_assert(kAcquireMargin >= kAcquireMaxOffset + kAcquireMaxDelay - kAcquireBackoff - kTrackBackoff);
// 追蹤時確認下一幀的前導符號：延遲視窗（長 32 點）起點的範圍，相對於 FFT 視窗起點。第一條路徑理應在 +16，
// 這個範圍容許它前後各差 40 點——process_frame 的定時修正處理得了這麼多。差得更遠的（對方的音效卡掉了一小段、
// 或是另一次發射剛好在附近開始）不算確認到，交給搜尋重新捕獲，那邊的定時不受這個限制。
constexpr int kTrackDetectFirst = -24;
constexpr int kTrackDetectLast = 24;
constexpr int kTailSegment = 18;      // 解出來的幀：以這麼多個符號為一段，從尾端往回看導頻在不在
// 回頭補救中途斷開的幀（RxTuning::mend_steps、Receiver::mend_frame）
constexpr int kMendMinStep = 12;      // 下一幀的起點偏離整數幀至少這麼多個取樣才當成斷開過；更小的，定時追蹤自己跟得上
constexpr int kMendMaxStep = 2400;    // 最多 0.2 秒；再長的不是音訊卡了一下
constexpr int kMendMinRows = 4;       // 斷開之後至少還有這麼多個酬載符號：導頻的三種位置各有一個符號，兩段才能各自估通道
// 接起來之後，各對導頻的相關值加起來至少要比整幀都照原來的對時多這麼多才算數（完整的一對約 0.5–0.9，只有雜訊的約 0.35、方向隨機）。
// 斷開的長度不到一個循環字首時，照原來的對時解，斷開前後各自的導頻都還相干，差別只在跨過斷點的那三對。
constexpr double kMendGain = 1.5;
constexpr double kDipShare = 0.5;     // 導頻中間有連著三對對不上：它們加起來不到平常三對的這個比例（pilot_dip）
constexpr double kTrendKeep = 0.9;    // 取樣時脈差的估計：每過一幀，之前的份量乘上這個數
constexpr double kTrendMinWeight = 2.5;  // 累積到三幀左右才用
constexpr float kLostNoise = 1e6f;    // 斷開的地方沒有訊號的那幾個符號：格點的雜訊給這麼大，等於告訴解碼器「這裡沒有收到」
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
// 一對符號（第 j 個與第 j+3 個酬載符號）正規化的相關值；其中一個完全沒有訊號時是 0。
// 第 j 個取自 first、第 j+3 個取自 second：平常是同一個陣列；補救中途斷開的幀時，前後兩段的對時不同（Receiver::mend_frame）。
cd pilot_pair(const SymbolGrid& first, const SymbolGrid& second, int j, const std::array<int, 3>* skip = nullptr) {
    const int ra = j + kPreambleSymbols, rb = ra + 3;
    const float la = pilot_clip_level(first, ra, j), lb = pilot_clip_level(second, rb, j);
    cd acc(0, 0);
    double ea = 0.0, eb = 0.0;
    for (int c = 0; c < kCarriers; ++c) {
        if (!is_pilot(j, kFirstBin + c)) continue;  // 第 j 與第 j+3 個符號的導頻在同樣的格子上
        if (skip != nullptr && (c == (*skip)[0] || c == (*skip)[1] || c == (*skip)[2])) continue;
        const cd a = limited(first.at(ra, c), la);
        const cd b = limited(second.at(rb, c), lb);
        acc += b * std::conj(a);
        ea += std::norm(a);
        eb += std::norm(b);
    }
    return (ea > 0.0 && eb > 0.0) ? acc / std::sqrt(ea * eb) : cd(0, 0);
}

double pilot_coherence(const SymbolGrid& grid, int first, int last, const std::array<int, 3>* skip = nullptr) {
    cd total(0, 0);
    int pairs = 0;
    for (int j = first; j + 3 < last; ++j) {
        ++pairs;
        total += pilot_pair(grid, grid, j, skip);
    }
    return pairs > 0 ? std::abs(total) / pairs : 0.0;
}

// 導頻整體相干，中間卻有連著三對對不上。一幀收到一半、之後的符號整批晚到或早到（音訊斷開過）時就是這個樣子：
// 跨過斷點的那三對（前一個符號在斷點之前、後一個在之後）相位差了一個隨頻率變的斜率，加不起來；斷點前後各自的對仍然相干——
// 之後的符號就算錯位到夾著前後兩個符號，導頻「還在」，只是和之前的對不上。只看導頻在不在，看不出這種幀。
// 衰落的谷底與突波也會讓幾對對不上，所以這只算「可疑」（見 Receiver::process_frame：可疑的幀先留著不交）。
bool pilot_dip(const SymbolGrid& grid, int payload_symbols) {
    const int pairs = payload_symbols - 3;
    if (pairs < 6) return false;
    std::vector<cd> value(static_cast<std::size_t>(pairs));
    cd total(0, 0);
    for (int j = 0; j < pairs; ++j) {
        value[static_cast<std::size_t>(j)] = pilot_pair(grid, grid, j);
        total += value[static_cast<std::size_t>(j)];
    }
    const double size = std::abs(total);
    if (size <= 0.0) return false;
    const cd toward = std::conj(total / size);
    double lowest = 3.0;
    for (int j = 0; j + 3 <= pairs; ++j) {
        const cd three = value[static_cast<std::size_t>(j)] + value[static_cast<std::size_t>(j + 1)] + value[static_cast<std::size_t>(j + 2)];
        lowest = std::min(lowest, (three * toward).real());
    }
    return lowest < kDipShare * 3.0 * size / pairs;
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

// ───── 沒有散佈導頻的幀（Mode::scattered_pilots 為 false；給 FM 語音通道用）─────
// 這種幀的酬載每個載波都是資料，前後各有一個已知的符號：開頭的 S1，與結尾再送一次的 S1（結尾的參考符號）。
// FM 的音訊通道在一幀之內不變（沒有衰落、沒有頻偏），會變的只有兩件事：兩邊音效卡的取樣時脈差一點，
// 所以訊號一個符號比一個符號晚到（或早到）；以及殘餘的一點點共同相位。兩者都隨時間線性變化，
// 由頭尾兩個參考符號量出來，中間的符號照比例內插（Receiver::process_frame_free）。
// 音訊在送進來之前被整段移過頻率時，共同的相位不再是一點點，而是跟著時間差成比例地轉（RxTuning::band_shift_hz）。
constexpr int kNullFirst = 58;        // 頻帶上方沒用到的頻率從第幾格開始（共 FreeTrack::kNullBins 格，到第 85 格）
constexpr int kCyclicFirst = 16;      // 循環字首（64 個取樣）裡拿來和符號尾巴比的那一段：前面一截留給通道的拖尾，
constexpr int kCyclicLast = 56;       // 最後幾個留給定時誤差
constexpr double kDriftRange = 14.0;  // 頭尾兩個參考符號之間的時間差找到 ±這麼多個取樣（4.4 秒的幀、時脈差 250 ppm 是 13 個）
constexpr double kFreeReliable = 0.5;       // 參考符號和預測的對得上（正規化相關值）才採用；結尾的對不上就是這一幀沒有送完
constexpr double kFreeFreshStart = 0.8;     // 頭尾兩個參考符號彼此對得這麼好、卻和預測的對不上：留著的通道不對了，重新開始
constexpr int kFreeAverageFrames = 8;       // 通道最多平均這麼多幀
constexpr double kFreeMaxCellSnrDb = 35.0;  // 交給解碼器的雜訊不低於這個訊雜比對應的值：削波、量化這些量不到的失真大約在這裡
constexpr double kFreeNullFloor = 1e-7;     // 頻帶外的能量至少是頻帶內的這個比例才拿來用（檔案對檔案、沒有雜訊時那裡是空的）
// 被雜訊的突波打壞的符號（見 process_frame_free）：頻帶上方的雜訊是平常的這麼多倍以上、估出來的雜訊至少是訊號的這個比例，
// 才拿收到的總能量和預期的比；多出、少掉這麼多才調高那個符號的雜訊。
constexpr double kFreeBurstRelative = 1.5;
constexpr double kFreeBurstShare = 0.02;
constexpr double kFreeExcess = 1.3;
constexpr double kFreeDeficit = 0.7;
// 收到的能量不到預期的這個比例：那個符號根本沒有訊號（對方送到一半就停了）。不管雜訊看起來正不正常，都不能把它當成收到的值。
// 乾淨的訊號一個符號的能量起伏不會到這麼低。
constexpr double kFreeAbsent = 0.25;
// 各載波的雜訊形狀（見 process_frame_free「各載波的雜訊」與 Receiver::carrier_shape）
constexpr double kShapeMaxSnrDb = 10.0;    // 一個符號的訊雜比（每個格點）在這以上就不量：雜訊太小，量不準，各載波的份量本來也差不多
constexpr double kShapeClassStepDb = 4.0;  // 往下每這麼多 dB 一級：6–10、2–6、−2–2、−2 以下（FreeTrack::kShapeClasses 級）
constexpr double kShapeForget = 0.7;       // 每過一幀，之前累計的份量乘上這個數（大約是最近三幀）
constexpr double kShapeMinRows = 6.0;      // 一級裡累計的符號不到這麼多個，就不用它自己量到的
constexpr int kShapeBands = 7;             // 49 個載波分成這麼多段，每段各自平均
constexpr double kShapeFloor = 0.03;       // 某一段扣掉訊號之後剩下的比各段平均的這個比例還小（或是負的）：當成這麼多
constexpr double kShapeMaxTiltDb = 24.0;   // 最高與最低的一段最多差這麼多
static_assert(kCarriers % kShapeBands == 0);
// 找訊號之前把輸入的頻譜拉平（Receiver::flatten）
constexpr double kFlatCentreHz = 1500.0;  // 頻帶的正中間：拉到頻譜的重心落在這裡
constexpr double kFlatSeconds = 1.0;      // 自相關是最近這麼久的指數平均
constexpr int kFlatEvery = 256;           // 每這麼多個取樣重新算一次 a
constexpr double kFlatMin = 0.5;          // 算出來的 a 不到這個數就不動（重心只偏一點：訊號自己的頻率響應、衰落，不是雜訊偏低頻）
constexpr double kFlatMax = 0.95;         // a 的上限（接近 1 時是微分器，頻帶兩端差 17 dB）
constexpr double kFlatStep = 0.25;        // 每次往算出來的值靠近這個比例

// 兩個通道估計（49 個載波）差多少：b ≈ gain · a · exp(−j2πk·tau/N + j·phi)。
// tau 是 b 比 a 晚幾個取樣，phi 是共同的相位，rho 是對齊之後的正規化相關值（同一個通道、訊號乾淨時接近 1）。
struct TimeShift {
    double tau = 0.0;
    double phi = 0.0;
    double rho = 0.0;
    double gain = 1.0;
};

TimeShift time_shift_between(const cf* a, const cf* b) {
    std::array<cd, kCarriers> q;
    double ea = 0.0, eb = 0.0;
    for (int c = 0; c < kCarriers; ++c) {
        q[static_cast<std::size_t>(c)] = cd(b[c]) * std::conj(cd(a[c]));
        ea += std::norm(a[c]);
        eb += std::norm(b[c]);
    }
    TimeShift out;
    if (ea <= 0.0 || eb <= 0.0) return out;
    // 把 b 往回轉 tau 個取樣之後和 a 的相關
    auto sum_at = [&](double tau) {
        const double step = kTwoPi * tau / kFftSize;
        cd rot = std::polar(1.0, step * kFirstBin);
        const cd inc = std::polar(1.0, step);
        cd acc(0, 0);
        for (int c = 0; c < kCarriers; ++c) {
            acc += q[static_cast<std::size_t>(c)] * rot;
            rot *= inc;
        }
        return acc;
    };
    // 先每 0.25 個取樣找一次（相關值的主瓣寬約 5 個取樣），再在最高的那一格附近每 0.01 個取樣找一次
    constexpr double kCoarse = 0.25, kFine = 0.01;
    double best = -1.0, best_tau = 0.0;
    const int coarse_steps = static_cast<int>(kDriftRange / kCoarse);
    for (int i = -coarse_steps; i <= coarse_steps; ++i) {
        const double m = std::abs(sum_at(i * kCoarse));
        if (m > best) {
            best = m;
            best_tau = i * kCoarse;
        }
    }
    const double center = best_tau;
    const int fine_steps = static_cast<int>(kCoarse / kFine);
    for (int i = -fine_steps; i <= fine_steps; ++i) {
        const double tau = center + i * kFine;
        const double m = std::abs(sum_at(tau));
        if (m > best) {
            best = m;
            best_tau = tau;
        }
    }
    const cd sum = sum_at(best_tau);
    out.tau = best_tau;
    out.phi = std::arg(sum);
    out.rho = std::abs(sum) / std::sqrt(ea * eb);
    out.gain = std::abs(sum) / ea;
    return out;
}

// 由一列參考符號估出的通道（49 個載波，已經除掉已知值、也拿掉了視窗提前量的相位）求第一條有份量的路徑
// 相對於目前時間基準的延遲（取樣數，可為負）。做法和 Receiver::s1_path_delays 相同。
double response_first_path(const cf* h) {
    cf x[kFftSize];
    std::fill(x, x + kFftSize, cf(0, 0));
    for (int c = 0; c < kCarriers; ++c) {
        const float hann = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * (c + 1) / (kCarriers + 1)));
        x[kFirstBin + c] = h[c] * hann;
    }
    fft256().inverse(x);
    std::array<double, kFftSize> profile{};
    for (int t = 0; t < kFftSize; ++t) profile[static_cast<std::size_t>(t)] = std::norm(x[t]);
    const int peak = static_cast<int>(std::max_element(profile.begin(), profile.end()) - profile.begin());
    std::array<double, kFftSize> sorted = profile;
    std::nth_element(sorted.begin(), sorted.begin() + kFftSize / 2, sorted.end());
    const double threshold = std::max(0.2 * profile[static_cast<std::size_t>(peak)], 12.0 * sorted[kFftSize / 2]);
    int first = peak;
    for (int back = 64; back > 0; --back) {
        const int t = ((peak - back) % kFftSize + kFftSize) % kFftSize;
        if (profile[static_cast<std::size_t>(t)] >= threshold) {
            first = peak - back;
            break;
        }
    }
    double delay = first + (first == peak ? 0.0 : 5.0);  // 加窗後主瓣在門檻處提前約 5 個取樣
    if (delay >= kFftSize / 2.0) delay -= kFftSize;
    return delay;
}

// 一列已知的符號除掉已知值之後，相鄰載波的通道估計是不是幾乎相同（正規化的相關值）。
// 真正的參考符號乘上的是平滑的通道：相鄰兩格只差一點點相位（定時偏了也只是每一格多轉同樣的角度），相關值接近 S/(S+N)。
// 那裡如果不是這個符號（別種訊號的酬載、導頻加上資料），除完之後各格的相位是亂的，相關值在 0 附近。
double adjacent_coherence(const cf* h, int count) {
    cd acc(0, 0);
    double ea = 0.0, eb = 0.0;
    for (int i = 0; i + 1 < count; ++i) {
        acc += cd(h[i + 1]) * std::conj(cd(h[i]));
        ea += std::norm(h[i]);
        eb += std::norm(h[i + 1]);
    }
    return ea > 0.0 && eb > 0.0 ? std::abs(acc) / std::sqrt(ea * eb) : 0.0;
}

// 酬載的能量是不是分散在整個頻帶：49 個載波各自把這幾列的能量加起來，回傳最強的 3 個佔的比例。
// 真正的幀每個載波平均一樣強（3 個佔 6% 上下，音訊濾波器把頻帶邊緣壓低的話多一點）；
// 單頻載波，或只佔幾個載波的別種訊號，會超過一半。
double payload_top_share(const SymbolGrid& grid, int first_row, int last_row) {
    std::array<double, kCarriers> energy{};
    double total = 0.0;
    for (int r = first_row; r < last_row; ++r) {
        for (int c = 0; c < kCarriers; ++c) {
            const double e = std::norm(grid.at(r, c));
            energy[static_cast<std::size_t>(c)] += e;
            total += e;
        }
    }
    if (total <= 0.0) return 1.0;
    std::partial_sort(energy.begin(), energy.begin() + 3, energy.end(), std::greater<double>());
    return (energy[0] + energy[1] + energy[2]) / total;
}

}  // namespace

Receiver::Receiver() {
    buf_.reserve(1 << 17);
    flat_.reserve(1 << 17);
    raw_.reserve(1 << 17);
}

void Receiver::reset() {
    analytic_.reset();
    buf_.clear();
    flat_.clear();
    flat_a_ = 0.0;
    flat_r0_ = 0.0;
    flat_r1_ = flat_r2_ = cd(0.0, 0.0);
    flat_prev_ = flat_prev2_ = cf(0.0f, 0.0f);
    flat_count_ = 0;
    raw_.clear();
    free_ = FreeTrack{};
    base_ = 0;
    total_in_ = 0;
    level_ = 0.0;
    frames_.clear();
    earlier_.clear();
    frames_decoded_ = 0;
    frames_failed_ = 0;
    frames_skipped_ = 0;
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
    mend_ = Mend{};
    trend_sum_ = trend_weight_ = 0.0;
    from_previous_ = false;
    last_mode_ = nullptr;
    last_end_ = 0;
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

void Receiver::push(std::span<const float> raw) {
    if (raw.empty()) return;
    // 非有限值與離譜的值不進濾波器、不進任何滑動平均（見 dsp.hpp 的 clean_sample）
    clean_samples(raw, clean_);
    const std::span<const float> samples = clean_;
    // 輸入音量：時間常數約 0.1 秒的指數平均
    const double alpha = 1.0 / (0.1 * kSampleRate);
    for (float s : samples) level_ += alpha * (static_cast<double>(s) * s - level_);
    const std::size_t had = buf_.size();
    analytic_.process(samples, buf_);
    flatten(had);
    raw_.insert(raw_.end(), samples.begin(), samples.end());  // 和 buf_ 一樣長、同樣的編號
    total_in_ += static_cast<std::int64_t>(samples.size());
    while (step()) {
    }
    trim();
}

void Receiver::finish() { drop_mend(); }

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
    s.frames_skipped = frames_skipped_;
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
    if (mend_.valid) {
        // 等著補救的那一幀：下一幀的前導符號最晚會出現在它的結尾之後 kMendMaxStep 個取樣。找 S0 的位置過了那裡、那之前也沒有
        // 還在等判定的候選位置，就不等了（留著沒交的照原樣交出去）。等的時候它的取樣要留著，往前多留 kMendMaxStep
        // （斷開之後早到的那一段）。
        const std::int64_t limit = mend_.t0 + mend_.mode->frame_samples() + kMendMaxStep;
        const bool scanning = state_ == State::Search || state_ == State::Receive;
        const bool waiting = (cand_ready_ && cand_pos_ <= limit) || (peak_active_ && peak_pos_ <= limit) || pending_start_ >= 0;
        if (scanning && scan_pos_ > limit && !waiting) drop_mend();
        else keep = std::min(keep, mend_.t0 - kMendMaxStep - 2048);
    }
    if (keep - base_ > kBufferSlack) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(keep - base_));
        flat_.erase(flat_.begin(), flat_.begin() + static_cast<std::ptrdiff_t>(keep - base_));
        raw_.erase(raw_.begin(), raw_.begin() + static_cast<std::ptrdiff_t>(keep - base_));
        base_ = keep;
    }
}

// buf_ 裡從第 from 個起是新進來的：算出它們拉平之後的值，接在 flat_ 後面。
// 濾波器是一階的：y[n] = x[n] − a·x[n−1]，a 愈大愈壓低頻（a = 0 不動）。a 怎麼定：讓濾過之後的頻譜重心落在頻帶正中間。
// 濾過之後延遲 1 的自相關是 (1 + a²)·r1 − a·(r0 + r2)，它的角度就是重心的頻率；r0、r1、r2 是輸入延遲 0、1、2 的自相關，
// 用最近約一秒的指數平均。要它的角度等於正中間的頻率，是 a 的二次方程式，兩個根互為倒數，取 0 到 1 之間的那一個。
// 輸入的重心本來就不低於正中間（短波的雜訊、夠強的訊號）時 a = 0，flat_ 和 buf_ 完全相同。
// 這裡不知道、也不必知道接的是哪一台電台：雜訊偏多少，就拉多少。
void Receiver::flatten(std::size_t from) {
    const double alpha = 1.0 / (kFlatSeconds * kSampleRate);
    const double theta = kTwoPi * kFlatCentreHz / kSampleRate;
    const cd turn = std::polar(1.0, -theta);
    for (std::size_t i = from; i < buf_.size(); ++i) {
        const cf x = buf_[i];
        if (tuning_.flatten_search) {
            flat_r0_ += alpha * (std::norm(x) - flat_r0_);
            flat_r1_ += alpha * (cd(x) * std::conj(cd(flat_prev_)) - flat_r1_);
            flat_r2_ += alpha * (cd(x) * std::conj(cd(flat_prev2_)) - flat_r2_);
            if (++flat_count_ >= kFlatEvery) {
                flat_count_ = 0;
                double target = 0.0;
                const double a1 = (flat_r1_ * turn).imag();                           // 負的：重心比正中間低
                const double b1 = (flat_r2_ * turn).imag() - flat_r0_ * std::sin(theta);
                if (a1 < 0.0 && flat_r0_ > 0.0) {
                    const double disc = b1 * b1 - 4.0 * a1 * a1;
                    target = kFlatMax;                                                // 沒有實根：一階的濾波器拉到底也不夠
                    if (disc >= 0.0 && b1 < 0.0) target = std::min((b1 + std::sqrt(disc)) / (2.0 * a1), kFlatMax);
                    if (target < kFlatMin) target = 0.0;
                }
                flat_a_ += kFlatStep * (target - flat_a_);
                if (flat_a_ < 1e-3) flat_a_ = 0.0;
            }
        } else {
            flat_a_ = 0.0;
        }
        flat_.push_back(flat_a_ > 0.0 ? x - static_cast<float>(flat_a_) * flat_prev_ : x);
        flat_prev2_ = flat_prev_;
        flat_prev_ = x;
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
                const bool there = mode_->scattered_pilots ? pilots_present(t0_, cfo_, *mode_) : signal_present(t0_, *mode_);
                if (!there) {
                    if (trace_) std::fprintf(stderr, "[rx] 幀起點 %lld 之後沒有%s，放棄\n", static_cast<long long>(t0_), mode_->scattered_pilots ? "導頻" : "訊號");
                    mode_ = nullptr;
                    enter_search(rescan_from_);
                    return true;
                }
            }
            if (end() < t0_ + mode_->frame_samples() + 2 * kCyclicPrefix) return false;
            if (mode_->scattered_pilots) process_frame();
            else process_frame_free();
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
            // 需要 S0、S1 與驗證用的酬載符號，外加定時修正的餘裕（kAcquireMargin：定時最多往後移這麼多）
            if (end() < cand_pos_ + (kPreambleSymbols + kVerifySymbols) * kSymbolSamples + kAcquireMargin) return false;
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
                const cd a = flat_sample(d + m), b = flat_sample(d + m + kScLag);
                sc_p_ += std::conj(a) * b;
                sc_e1_ += std::norm(a);
                sc_e2_ += std::norm(b);
            }
            sc_valid_ = true;
            sc_age_ = 0;
        } else {
            // 由 d−1 的結果更新：去掉最舊的一項，加入最新的一項
            const cd a0 = flat_sample(d - 1), b0 = flat_sample(d - 1 + kScLag);
            const cd a1 = flat_sample(d - 1 + kScLength), b1 = flat_sample(d - 1 + kScLength + kScLag);
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
// flat 是 true 時取拉平過的那一份（flat_）：找前導符號、確認訊號在不在的那幾步用。
void Receiver::fft_window(std::int64_t start, double cfo, cf* out, bool flat) const {
    assert(start >= base_ && start + kFftSize <= end());
    // 正式版沒有 assert：萬一呼叫端算錯了範圍，寧可當成那裡沒有訊號（全 0），也不要讀到緩衝區外面
    if (start < base_ || start + kFftSize > end()) {
        std::fill(out, out + kFftSize, cf(0.0f, 0.0f));
        return;
    }
    const double step = -kTwoPi * cfo;
    // 相位只取小數部分，避免取樣編號很大時損失精度
    double phase0 = std::fmod(step * static_cast<double>(start % (1LL << 40)), kTwoPi);
    cd rot = std::polar(1.0, phase0);
    const cd inc = std::polar(1.0, step);
    const cf* in = flat ? &flat_[static_cast<std::size_t>(start - base_)] : &buf_[static_cast<std::size_t>(start - base_)];
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
//
// S0、S1 的 FFT 取的是拉平過的取樣（flatten）：視窗外的能量密度就是雜訊，雜訊偏低頻時低的那幾個載波會把它整個抬高。
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
    fft_window(s0_window, cfo, y0, true);
    fft_window(s0_window + kSymbolSamples, cfo, y1, true);

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
    fft_window(s1_window, cfo, y, true);
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
    for (int offset : {0, -kAcquireMaxOffset / 2, kAcquireMaxOffset / 2, -kAcquireMaxOffset, kAcquireMaxOffset}) {
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

    // 沒有散佈導頻的幀是給 FM 用的：FM 解調出來的音訊沒有頻偏，量到的只是雜訊造成的誤差，而且這種幀一點頻偏都禁不起
    // （頭尾兩個參考符號之間相位只要轉超過半圈就接不起來），所以一律當成 0；看起來偏了好幾格的不是這種訊號。
    const bool free = !mode->scattered_pilots;
    if (free && chk.shift != 0) {
        if (trace_) std::fprintf(stderr, "[rx]   模式 %d 的幀不該有頻偏，不收\n", mode->id);
        return false;
    }
    double cfo = free ? 0.0 : eps + static_cast<double>(chk.shift) / kFftSize;
    const double verify_need = free ? tuning_.verify_cyclic : tuning_.verify_pilots;
    const std::int64_t w1 = w0 + kSymbolSamples;
    const PathDelays paths = s1_path_delays(w1, cfo, chk.mode_id);
    // 路徑的 S1 有效部分從 w1 + 延遲 開始，往前推一個循環字首和一個符號就是 S0 的起點
    auto frame_start = [&](double delay) {
        const double limited = std::clamp(delay, static_cast<double>(kAcquireMinDelay), static_cast<double>(kAcquireMaxDelay));
        return w1 + std::llround(limited) - kCyclicPrefix - kSymbolSamples;
    };
    double tau = paths.first;
    std::int64_t t0 = frame_start(tau);
    if (t0 + kCyclicPrefix - kTrackBackoff < base_) return false;

    // 第二道驗證：酬載開頭的導頻要與 S1 估出的通道相符。雜訊偶爾會通過前導符號的檢查，
    // 但不會連導頻都對得上；有了這一步，前面的門檻可以放低，弱訊號比較不會漏掉。
    PilotCheck pilots = verify_frame_start(t0, cfo, *mode);
    if (std::fabs(paths.first - paths.peak) > 8.0) {
        // 「第一條路徑」可能只是雜訊：以最強路徑為準再驗證一次，取相關較高的定時
        const std::int64_t alt = frame_start(paths.peak);
        if (alt + kCyclicPrefix - kTrackBackoff >= base_) {
            const PilotCheck other = verify_frame_start(alt, cfo, *mode);
            if (other.rho > pilots.rho) {
                pilots = other;
                tau = paths.peak;
                t0 = alt;
            }
        }
    }
    if (trace_) {
        std::fprintf(stderr, "[rx] %s：模式 %d、頻偏 %+.2f Hz（導頻修正 %+.2f）、S1 第一條路徑 %+.1f、幀起點 %lld、導頻相關 %.2f\n",
                     pilots.rho >= verify_need ? "捕獲" : "導頻不符，放棄", mode->id, cfo * kSampleRate,
                     pilots.cfo_delta * kSampleRate, tau, static_cast<long long>(t0), pilots.rho);
    }
    if (pilots.rho < verify_need) return false;
    // 已經收完的幀不再收一次。搜尋都從上一幀的前導符號之後開始，候選位置往回最多只看到那之前 248 個取樣，
    // 照理不會再找到同一幀；這一關是保險：不管流程怎麼走，處理過的幀起點只會愈來愈後面，
    // 所以手上的取樣一定處理得完，不會在同樣幾個位置之間來回。
    if (t0 < done_start_ + kCyclicPrefix) {
        if (trace_) std::fprintf(stderr, "[rx]   幀起點 %lld 的那一幀已經收過了，不再收一次\n", static_cast<long long>(t0));
        return false;
    }
    if (receiving && free && mode == mode_ && free_.frames > 0 && std::llabs(t0 - t0_) <= kCyclicPrefix / 2) {
        // 這個前導符號就是正在收的這一幀自己的（照節拍接著收、前導符號當時沒確認到，搜尋後來又找到它）。
        // 它只說明這一幀確實在；通道、時脈差、幀的起點都已經由前幾幀累積得很準，不能因為它就全部重來——
        // 重新捕獲只靠這一個前導符號，而它正是剛才沒確認到的那一個（多半被雜訊打壞了）。當成確認，接著收。
        if (trace_) std::fprintf(stderr, "[rx]   幀起點 %lld 就是正在收的這一幀：當成確認，不重新開始\n", static_cast<long long>(t0));
        confirmed_ = true;
        frame_evidence_ = std::max(frame_evidence_, chk.score);
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
    // 上一幀沒有交出去（或留著還沒交），而這一幀的起點不在它之後整數幀的位置：它可能是中途斷開了，先回頭補救
    // （要趁這一幀還沒處理）。沒補救成的話，留著的照原樣交出去。
    const bool mended = mend_.valid && mend_frame(found);
    drop_mend();
    // 這一幀如果不是接在剛才那一幀後面不遠（同一個模式、差不到 kMendMaxStep），就是另一次發射：取樣時脈差多少要重新量
    if (found.mode != last_mode_ || std::llabs(found.t0 - last_end_) > kMendMaxStep) trend_sum_ = trend_weight_ = 0.0;
    from_previous_ = false;
    mode_ = found.mode;
    cfo_ = found.cfo;
    t0_ = found.t0;
    free_ = FreeTrack{};  // 另一次發射：通道、時脈差都重新量
    state_ = State::Receive;
    confirmed_ = true;
    presence_checked_ = true;
    rescan_from_ = found.t0 + kSymbolSamples;  // 這一幀若沒解出來、也沒有下一幀，就從這個前導符號之後重新搜尋
    unconfirmed_run_ = 0;
    // 補救回來的那一幀就是這一幀的上一幀：它的標頭正確，或者導頻從頭到尾都很清楚，和平常「上一幀解出來了」一樣
    last_ok_ = mended;
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

SymbolGrid Receiver::demodulate(std::int64_t t0, double cfo, int rows, bool flat) const {
    SymbolGrid grid(rows);
    // 視窗提前 kTrackBackoff 個取樣，會讓第 k 格多出 exp(−j2πk·提前量/N) 的相位，這裡乘回去
    std::array<cf, kCarriers> derotate;
    for (int c = 0; c < kCarriers; ++c) {
        derotate[static_cast<std::size_t>(c)] = std::polar(1.0f, static_cast<float>(kTwoPi * (kFirstBin + c) * kTrackBackoff / kFftSize));
    }
    cf y[kFftSize];
    for (int r = 0; r < rows; ++r) {
        fft_window(t0 + static_cast<std::int64_t>(r) * kSymbolSamples + kCyclicPrefix - kTrackBackoff, cfo, y, flat);
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

// ───── 沒有散佈導頻的幀 ─────

// 循環字首和符號的尾巴是不是相同：每個符號開頭的 64 個取樣是它最後 64 個取樣的複本，相隔 256 個取樣。
// 每個符號各自算出正規化的相關值再平均（每個符號的份量相同，讀不到的符號算 0）。有訊號而且位置對的時候
// 接近 S/(S+N)；只有雜訊時在 0 附近（一個符號約 ±0.3，平均 12 個符號約 0.1、36 個約 0.06）。
// 不必知道酬載放了什麼，所以沒有導頻的幀靠它判斷訊號在不在、送到哪裡為止。
// 單頻載波的循環字首也和尾巴相同，要另外用 payload_top_share 擋。
// 讀的是拉平過的取樣（flatten）：雜訊偏低頻時，原來的取樣算出來的值會被低頻的雜訊壓低。
double Receiver::cyclic_coherence(std::int64_t t0, int first_row, int last_row) const {
    cd total(0, 0);
    int rows = 0;
    for (int r = first_row; r < last_row; ++r) {
        ++rows;
        const std::int64_t s = t0 + static_cast<std::int64_t>(r) * kSymbolSamples;
        if (s + kCyclicFirst < base_ || s + kCyclicLast + kFftSize > end()) continue;
        cd acc(0, 0);
        double ea = 0.0, eb = 0.0;
        for (int n = kCyclicFirst; n < kCyclicLast; ++n) {
            const cd a = flat_sample(s + n), b = flat_sample(s + n + kFftSize);
            acc += b * std::conj(a);
            ea += std::norm(a);
            eb += std::norm(b);
        }
        if (ea > 0.0 && eb > 0.0) total += acc / std::sqrt(ea * eb);
    }
    return rows > 0 ? std::abs(total) / rows : 0.0;
}

// 一個符號裡訊號的功率（解析訊號每個取樣的均方值）：循環字首乘上符號尾巴對應的取樣再平均。兩邊的訊號相同、雜訊無關，
// 所以雜訊不會讓這個值偏高（只會讓它起伏）；訊號被雜訊壓掉的符號，量到的就少。讀不到時回傳 false。
bool Receiver::cyclic_signal(std::int64_t symbol_start, double& power) const {
    if (symbol_start + kCyclicFirst < base_ || symbol_start + kCyclicLast + kFftSize > end()) return false;
    double acc = 0.0;
    for (int n = kCyclicFirst; n < kCyclicLast; ++n) {
        const cd a = sample(symbol_start + n), b = sample(symbol_start + n + kFftSize);
        acc += (b * std::conj(a)).real();
    }
    power = acc / (kCyclicLast - kCyclicFirst);
    return true;
}

// 第 k 級符號的雜訊在各個載波上的形狀（49 個值，平均是 1）。量到的還不夠多時回傳 false。
// free_.shape_power 是那一級的符號每個載波收到的功率；訊號的總量是 free_.shape_signal，照通道估計（channel，總功率 channel_power）
// 分給各個載波，扣掉之後剩下的是雜訊。一個載波一個載波看起伏太大，所以分成 kShapeBands 段各自平均，再配一條直線
// （縱軸是雜訊的對數，橫軸是第幾段）；雜訊比訊號小很多的那幾段起伏大，份量就輕。直線的斜率不到自己誤差的兩倍，就當成是平的。
// tilt_db 是配出來最高的一段比最低的一段多幾 dB（低頻比較吵時是負的）。
bool Receiver::carrier_shape(int k, const std::array<cf, kCarriers>& channel, double channel_power, std::array<float, kCarriers>& out,
                             double& tilt_db) const {
    const std::size_t i = static_cast<std::size_t>(k);
    const double rows = free_.shape_rows[i];
    tilt_db = 0.0;
    if (rows < kShapeMinRows || !(channel_power > 0.0)) return false;
    const double signal = std::max(free_.shape_signal[i], 0.0);
    constexpr int kPer = kCarriers / kShapeBands;
    std::array<double, kShapeBands> noise{}, total{};
    double mean = 0.0;
    for (int b = 0; b < kShapeBands; ++b) {
        const std::size_t j = static_cast<std::size_t>(b);
        for (int c = b * kPer; c < (b + 1) * kPer; ++c) {
            const double p = free_.shape_power[i][static_cast<std::size_t>(c)];
            total[j] += p;
            noise[j] += p - signal * std::norm(channel[static_cast<std::size_t>(c)]) / channel_power;
        }
        total[j] /= kPer;
        noise[j] /= kPer;
        mean += noise[j] / kShapeBands;
    }
    if (!(mean > 0.0)) return false;
    double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double cells = kPer * rows;  // 一段裡平均了幾個格點
    for (int b = 0; b < kShapeBands; ++b) {
        const std::size_t j = static_cast<std::size_t>(b);
        const double level = std::max(noise[j], kShapeFloor * mean);
        const double sigma = total[j] / std::sqrt(cells) / level;  // 對數刻度上的誤差：一個格點的功率起伏和它自己一樣大
        const double w = 1.0 / (sigma * sigma);
        const double x = b - (kShapeBands - 1) / 2.0, y = std::log(level);
        sw += w;
        sx += w * x;
        sy += w * y;
        sxx += w * x * x;
        sxy += w * x * y;
    }
    const double det = sw * sxx - sx * sx;
    if (!(det > 0.0)) return false;
    double slope = (sw * sxy - sx * sy) / det;  // 每往上一段，雜訊的自然對數差多少
    if (slope * slope < 4.0 * sw / det) slope = 0.0;
    const double limit = kShapeMaxTiltDb * std::log(10.0) / 10.0 / (kShapeBands - 1);
    slope = std::clamp(slope, -limit, limit);
    double sum = 0.0;
    for (int c = 0; c < kCarriers; ++c) {
        const double value = std::exp(slope * (c - (kCarriers - 1) / 2.0) / kPer);
        out[static_cast<std::size_t>(c)] = static_cast<float>(value);
        sum += value;
    }
    for (float& v : out) v = static_cast<float>(v * kCarriers / sum);
    tilt_db = slope * (kShapeBands - 1) * 10.0 / std::log(10.0);
    return true;
}

// 開頭幾十個酬載符號裡訊號在不在（對應靠導頻的 pilots_present）。
bool Receiver::signal_present(std::int64_t t0, const Mode& mode) const {
    const int last = kPreambleSymbols + std::min(mode.payload_symbols, kPresenceSymbols);
    return cyclic_coherence(t0, kPreambleSymbols, last) > kPresenceThreshold;
}

// 捕獲後的第二道驗證。有散佈導頻的幀看酬載開頭的導頻（verify_pilots）。沒有的幀沒有導頻可看，改成三件事都要成立，
// 回傳其中最差的一個：
//   * S0 與 S1 除掉已知的序列之後，相鄰載波的通道估計幾乎相同（adjacent_coherence）。前導符號的判定（check_preamble）
//     看的是延遲域裡能量集不集中，別種訊號的酬載偶爾會過關：導頻每 6 格一個，剛好和某條 S1 序列在那幾格上同號時，
//     延遲域裡也會出現集中的峰。有導頻的幀靠導頻驗證擋掉這種位置；這裡靠的是那種符號相鄰的載波之間沒有關係。
//   * 酬載開頭那幾個符號的循環字首和符號的尾巴相同（cyclic_coherence）：後面確實接著同樣節拍的符號。
//   * 能量分散在整個頻帶：單頻載波的循環字首也和尾巴相同。
Receiver::PilotCheck Receiver::verify_frame_start(std::int64_t t0, double cfo, const Mode& mode) const {
    if (mode.scattered_pilots) return verify_pilots(t0, cfo, mode);
    PilotCheck out;
    const int last = kPreambleSymbols + std::min(mode.payload_symbols, kVerifySymbols);
    const SymbolGrid grid = demodulate(t0, 0.0, last, true);
    if (payload_top_share(grid, kPreambleSymbols, last) >= kPilotTopShare) return out;
    std::array<cf, kCarriers> h1;
    for (int c = 0; c < kCarriers; ++c) h1[static_cast<std::size_t>(c)] = grid.at(1, c) * (s1_value(mode.id, kFirstBin + c) > 0.0f ? 1.0f : -1.0f);
    std::array<cf, kS0Carriers> h0;
    for (int i = 0; i < kS0Carriers; ++i) h0[static_cast<std::size_t>(i)] = grid.at(0, 2 * i) * std::conj(s0_value(kFirstBin + 2 * i));
    out.rho = std::min({cyclic_coherence(t0, kPreambleSymbols, last), adjacent_coherence(h1.data(), kCarriers), adjacent_coherence(h0.data(), kS0Carriers)});
    return out;
}

// 每個符號在頻帶上方沒用到的那幾格（第 58–85 格，2719–3984 Hz）的能量：out[列·kNullBins + 第幾格]。
// 取的是還沒濾波的取樣（解析濾波器把那一段濾掉了），視窗和 demodulate 的對齊：一個符號裡各個載波正交，
// 訊號自己不會漏到這幾格，聽到的就是那個符號的雜訊。讀不到的列留 0。
void Receiver::null_powers(std::int64_t t0, int rows, std::vector<float>& out) const {
    constexpr int kBins = FreeTrack::kNullBins;
    out.assign(static_cast<std::size_t>(rows) * kBins, 0.0f);
    const int delay = analytic_.delay_samples();
    cf y[kFftSize];
    for (int r = 0; r < rows; ++r) {
        const std::int64_t start = t0 + static_cast<std::int64_t>(r) * kSymbolSamples + kCyclicPrefix - kTrackBackoff - delay;
        if (start < base_ || start + kFftSize > end()) continue;
        const float* in = &raw_[static_cast<std::size_t>(start - base_)];
        for (int n = 0; n < kFftSize; ++n) y[n] = cf(in[n], 0.0f);
        fft256().forward(y);
        for (int i = 0; i < kBins; ++i) out[static_cast<std::size_t>(r * kBins + i)] = std::norm(y[kNullFirst + i]);
    }
}

// 收滿一幀沒有散佈導頻的幀之後：解調、估通道、解碼，再預測下一幀的位置。流程和 process_frame 相同，
// 靠導頻的地方換成：頭尾兩個參考符號（通道、時脈漂移、這一幀有沒有送完）、循環字首（訊號在不在）、
// 頻帶上方沒用到的頻率（每個符號的雜訊）。
void Receiver::process_frame_free() {
    drop_mend();  // 回頭補救中途斷開的幀只做了有散佈導頻的那一種；之前留著沒交的先交出去
    last_mode_ = nullptr;
    const Mode& mode = *mode_;
    const int rows = mode.frame_symbols();
    const int tail = rows - 1;      // 結尾的參考符號那一列
    const double span = tail - 1;   // 頭尾兩個參考符號相隔幾個符號
    const int payload_first = kPreambleSymbols, payload_last = kPreambleSymbols + mode.payload_symbols;
    const bool previous_ok = last_ok_;
    const std::int64_t t0_before = t0_;
    if (confirmed_) done_start_ = t0_before;
    cfo_ = 0.0;  // FM 解調出來的音訊沒有頻偏
    SymbolGrid grid = demodulate(t0_, 0.0, rows);
    auto turn = [](int c, double samples, double phase) {  // 第 c 個載波晚 samples 個取樣、相位多 phase
        return cf(std::polar(1.0, -kTwoPi * (kFirstBin + c) * samples / kFftSize + phase));
    };

    // 頭尾兩個參考符號各自量到的通道（含參考符號的振幅；資料格的振幅和它相同，所以等化時直接除就好）
    std::array<float, kCarriers> head_sign, tail_sign;
    for (int c = 0; c < kCarriers; ++c) {
        head_sign[static_cast<std::size_t>(c)] = s1_value(mode.id, kFirstBin + c) > 0.0f ? 1.0f : -1.0f;
        tail_sign[static_cast<std::size_t>(c)] = s1_value(kTailReferenceId, kFirstBin + c) > 0.0f ? 1.0f : -1.0f;
    }
    std::array<cf, kCarriers> h1, h2;
    auto take_references = [&] {
        for (int c = 0; c < kCarriers; ++c) {
            h1[static_cast<std::size_t>(c)] = grid.at(1, c) * head_sign[static_cast<std::size_t>(c)];
            h2[static_cast<std::size_t>(c)] = grid.at(tail, c) * tail_sign[static_cast<std::size_t>(c)];
        }
    };
    take_references();

    // 同一次發射的前幾幀留下了平均的通道與時脈漂移（free_）：照它們預測這一幀頭尾兩個參考符號會收到什麼，
    // 再量實際收到的和預測差多少（時間、相位、音量）。訊號忽強忽弱時，參考符號可能剛好落在訊號弱的那一刻；
    // 和預測對不上的那一個不用，改照預測走，兩個都對不上就整幀照預測。
    // 這次發射的第一幀沒有東西可以預測：開頭的參考符號就是基準，結尾的和它比。
    bool tracked = free_.frames > 0;
    std::array<cf, kCarriers> predicted;  // 預測開頭的參考符號量到的通道
    TimeShift head, end_shift;
    auto measure = [&] {
        if (tracked) {
            for (int c = 0; c < kCarriers; ++c) {
                predicted[static_cast<std::size_t>(c)] = free_.ref[static_cast<std::size_t>(c)] * turn(c, free_.pending_shift, free_.pending_phase);
            }
            head = time_shift_between(predicted.data(), h1.data());
        } else {
            predicted = h1;
            head = TimeShift{};
            head.rho = 1.0;
        }
        std::array<cf, kCarriers> at_tail;  // 預測結尾的參考符號：比開頭的再晚 drift × span 個取樣
        for (int c = 0; c < kCarriers; ++c) at_tail[static_cast<std::size_t>(c)] = predicted[static_cast<std::size_t>(c)] * turn(c, free_.drift * span, 0.0);
        end_shift = time_shift_between(at_tail.data(), h2.data());
    };
    measure();
    if (tracked && (head.rho < kFreeReliable || end_shift.rho < kFreeReliable) &&
        time_shift_between(h1.data(), h2.data()).rho >= kFreeFreshStart) {
        // 頭尾兩個參考符號都很清楚、彼此也對得上，卻和預測的不一樣：留著的通道不對了（對方換了設備或音量大改），重新開始
        if (trace_) std::fprintf(stderr, "[rx]   參考符號和前幾幀留下的通道對不上，重新開始估\n");
        free_.frames = 0;
        free_.drift = 0.0;
        free_.drift_count = 0;
        free_.pending_shift = free_.pending_phase = 0.0;
        tracked = false;
        measure();
    }

    // 定時：開頭的參考符號偏離基準 3 個取樣以上就移動視窗重新解調。第一幀看它的第一條路徑；之後看它比預測的晚多少。
    double moved_by = tracked ? (head.rho >= kFreeReliable ? head.tau : 0.0) : response_first_path(h1.data());
    moved_by = std::clamp(moved_by, -48.0, 48.0);
    std::int64_t window_moved = 0;  // 視窗實際移了幾個取樣
    if (std::fabs(moved_by) >= 3.0) {
        const std::int64_t shift = std::llround(moved_by);
        const std::int64_t moved = t0_ + shift;
        if (moved + kCyclicPrefix - kTrackBackoff >= base_ && moved + mode.frame_samples() + kCyclicPrefix <= end()) {
            t0_ = moved;
            window_moved = shift;
            if (tracked) free_.pending_shift -= static_cast<double>(shift);
            grid = demodulate(t0_, 0.0, rows);
            take_references();
            measure();
        }
    }

    const bool head_ok = head.rho >= kFreeReliable;
    const bool tail_ok = end_shift.rho >= kFreeReliable;  // 結尾的參考符號在：對方把這一幀送完了
    // 開頭與結尾相對於「預測的開頭」晚了多少、相位差多少；中間的符號照比例內插
    const double predicted_span = free_.drift * span;
    double tau0 = 0.0, tau1 = predicted_span, phi0 = 0.0, phi1 = 0.0;
    if (head_ok && tail_ok) {
        tau0 = head.tau;
        phi0 = head.phi;
        tau1 = end_shift.tau + predicted_span;
        phi1 = end_shift.phi;
    } else if (head_ok) {
        tau0 = head.tau;
        tau1 = tau0 + predicted_span;
        phi0 = phi1 = head.phi;
    } else if (tail_ok) {
        tau0 = end_shift.tau;
        tau1 = tau0 + predicted_span;
        phi0 = phi1 = end_shift.phi;
    }
    // 頭尾之間共同的相位轉了多少。平常（FM 的音訊沒有頻偏）只是殘餘的一點點：取最短的那一圈。
    // 音訊在送進來之前被整段移過頻率的話（RxTuning::band_shift_hz），時脈差會讓整個頻帶跟著時間差一起轉：
    // 每晚一個取樣多轉 −2π · band_shift_hz / 12000，長的幀、時脈差大時超過半圈。所以先照量到的時間差算出該轉多少，
    // 再取離它最近的那一圈；只有一個參考符號可用時就照算出來的。
    const double expected_dphi = -kTwoPi * tuning_.band_shift_hz / kSampleRate * (tau1 - tau0);
    const double dphi = head_ok && tail_ok ? expected_dphi + std::remainder(phi1 - phi0 - expected_dphi, kTwoPi) : expected_dphi;
    // 這一幀的音量是前幾幀平均的幾倍（對方或自己中途調了音量）
    double gain = 1.0;
    if (tracked && (head_ok || tail_ok)) gain = ((head_ok ? head.gain : 0.0) + (tail_ok ? end_shift.gain : 0.0)) / ((head_ok ? 1 : 0) + (tail_ok ? 1 : 0));

    // 這一幀的通道（對齊到預測的開頭）：前幾幀的平均，加上這一幀可用的參考符號（各自轉回開頭的時間）
    const int earlier = tracked ? 2 * free_.frames : 0;  // 前幾幀的平均相當於幾個參考符號
    const int fresh = (head_ok ? 1 : 0) + (tail_ok ? 1 : 0);
    const int references = std::max(earlier + fresh, 1);
    std::array<cf, kCarriers> base, back1{}, back2{};
    double power = 0.0;  // 一個符號所有載波的訊號功率
    for (int c = 0; c < kCarriers; ++c) {
        const std::size_t i = static_cast<std::size_t>(c);
        cd sum = tracked ? cd(predicted[i]) * (gain * earlier) : cd(0, 0);
        if (head_ok) {
            back1[i] = h1[i] * turn(c, -tau0, -phi0);
            sum += cd(back1[i]);
        }
        if (tail_ok) {
            back2[i] = h2[i] * turn(c, -tau1, -phi1);
            sum += cd(back2[i]);
        }
        base[i] = earlier + fresh > 0 ? cf(sum / static_cast<double>(references)) : predicted[i];
        power += std::norm(base[i]);
    }
    const double mean_power = power / kCarriers;
    // 雜訊：這一幀的參考符號和平均出來的通道差多少。n 個裡的一個和平均值的差，變異數是雜訊的 (1 − 1/n) 倍。
    double pair_var = free_.noise_var > 0.0f ? static_cast<double>(free_.noise_var) : 0.01 * mean_power;  // 每個格點的複數雜訊變異數（FFT 輸出的刻度）
    const bool measured = fresh > 0 && references > 1;
    if (measured) {
        double residual = 0.0;
        for (int c = 0; c < kCarriers; ++c) {
            const std::size_t i = static_cast<std::size_t>(c);
            if (head_ok) residual += std::norm(back1[i] - base[i]);
            if (tail_ok) residual += std::norm(back2[i] - base[i]);
        }
        pair_var = residual / (kCarriers * fresh * (1.0 - 1.0 / references));
        free_.noise_var = static_cast<float>(pair_var);
    }
    // 留給下一幀的：對齊到這一幀實際的開頭
    if (fresh > 0) {
        for (int c = 0; c < kCarriers; ++c) free_.ref[static_cast<std::size_t>(c)] = base[static_cast<std::size_t>(c)] * turn(c, tau0, phi0);
        free_.frames = std::min((tracked ? free_.frames : 0) + 1, kFreeAverageFrames);
    } else {
        free_.ref = predicted;
    }
    const double slope = (tau1 - tau0) / span;  // 這一幀裡每過一個符號晚幾個取樣
    if (head_ok && tail_ok) {
        free_.drift_count = std::min(free_.drift_count + 1, 4);
        free_.drift += (slope - free_.drift) / free_.drift_count;
    }

    // 每個符號的雜訊。訊號忽強忽弱時（車上、手持），FM 解調出來的雜訊跟著一個符號一個符號地變；整幀只用一個值的話，
    // 弱的那幾個符號會被當成和其他的一樣可信。頻帶上方沒用到的頻率只有雜訊，而且和頻帶內的雜訊同進退：
    // 每一格先除以它自己平常的大小（電台的音訊濾波器在那裡壓得多深都無所謂），平均起來就是「這個符號比平常吵幾倍」，
    // 再用參考符號量到的雜訊定出絕對的大小。
    std::vector<double> row_var(static_cast<std::size_t>(rows), pair_var);
    // 每個酬載符號收到的總能量是預期的幾倍（預期的是「通道 × 資料格的平均功率 1」）
    std::vector<double> energy_ratio(static_cast<std::size_t>(rows), 1.0);
    for (int row = payload_first; row < payload_last; ++row) {
        double energy = 0.0;
        for (int c = 0; c < kCarriers; ++c) energy += std::norm(grid.at(row, c));
        energy_ratio[static_cast<std::size_t>(row)] = power > 0.0 ? energy / power : 1.0;
    }
    bool profiled = false;
    if (tuning_.noise_map) {
        constexpr int kBins = FreeTrack::kNullBins;
        std::vector<float> nulls;
        null_powers(t0_, rows, nulls);
        if (fresh > 0) {  // 這裡確實有這一幀：算進「各格平常有多吵」
            for (int r = 0; r < rows; ++r) {
                for (int i = 0; i < kBins; ++i) free_.null_sum[static_cast<std::size_t>(i)] += nulls[static_cast<std::size_t>(r * kBins + i)];
            }
            free_.null_rows += rows;
        }
        std::array<double, kBins> level{};
        double level_total = 0.0;
        bool usable = free_.null_rows > 0;
        for (int i = 0; usable && i < kBins; ++i) {
            level[static_cast<std::size_t>(i)] = free_.null_sum[static_cast<std::size_t>(i)] / static_cast<double>(free_.null_rows);
            level_total += level[static_cast<std::size_t>(i)];
            if (!(level[static_cast<std::size_t>(i)] > 0.0)) usable = false;
        }
        // 解析訊號的功率是原來取樣的 4 倍（只留正頻率、振幅乘 2），比的時候換成同一個刻度
        if (usable && level_total >= kFreeNullFloor * power / 4.0) {
            std::vector<double> relative(static_cast<std::size_t>(rows), 1.0);  // 每個符號比平常吵幾倍
            for (int r = 0; r < rows; ++r) {
                double sum = 0.0;
                for (int i = 0; i < kBins; ++i) sum += nulls[static_cast<std::size_t>(r * kBins + i)] / level[static_cast<std::size_t>(i)];
                relative[static_cast<std::size_t>(r)] = sum / kBins;
            }
            if (measured) {
                free_.scale_num += pair_var * fresh;
                free_.scale_den += (head_ok ? relative[1] : 0.0) + (tail_ok ? relative[static_cast<std::size_t>(tail)] : 0.0);
            }
            if (free_.scale_den > 0.0) {
                const double scale = free_.scale_num / free_.scale_den;
                for (int r = 0; r < rows; ++r) row_var[static_cast<std::size_t>(r)] = scale * relative[static_cast<std::size_t>(r)];
                profiled = true;
                // 被雜訊的突波打壞的符號。FM 的訊號掉到門檻以下時出現的喀喀聲能量集中在低頻，頻帶上方聽到的比頻帶內少，
                // 上面的估計會偏低（最多差 10 dB）；訊號再弱，連訊號本身都會被壓掉。這兩種從收到的總能量看得出來：
                // 和「通道 × 資料格的平均功率 1」比，多出很多或少了很多。49 個資料格的能量本來就有起伏
                // （乾淨的訊號也偶爾差到三四成），所以只在頻帶上方顯示這個符號明顯比平常吵、估出來的雜訊也已經不小的時候才看。
                for (int row = payload_first; row < payload_last; ++row) {
                    const std::size_t r = static_cast<std::size_t>(row);
                    if (relative[r] < kFreeBurstRelative || row_var[r] < kFreeBurstShare * mean_power) continue;
                    const double ratio = energy_ratio[r];
                    if (ratio > kFreeExcess) row_var[r] = std::max(row_var[r], (ratio - 1.0) * mean_power);
                    else if (ratio < kFreeDeficit) row_var[r] = std::max(row_var[r], (1.0 - std::sqrt(ratio)) * (1.0 - std::sqrt(ratio)) * mean_power);
                }
            }
        }
    }
    // 根本沒有訊號的符號（見 kFreeAbsent）
    for (int row = payload_first; row < payload_last; ++row) {
        const std::size_t r = static_cast<std::size_t>(row);
        const double ratio = energy_ratio[r];
        if (ratio < kFreeAbsent) row_var[r] = std::max(row_var[r], (1.0 - std::sqrt(ratio)) * (1.0 - std::sqrt(ratio)) * mean_power);
    }
    const double floor_var = mean_power * std::pow(10.0, -kFreeMaxCellSnrDb / 10.0);
    double payload_var = 0.0;
    for (int row = payload_first; row < payload_last; ++row) payload_var += std::max(row_var[static_cast<std::size_t>(row)], floor_var);
    payload_var /= mode.payload_symbols;
    snr_db_ = static_cast<float>(10.0 * std::log10(power / (64.0 * payload_var)));  // 3 kHz 的雜訊頻寬是 64 格
    snr_valid_ = true;

    // 各載波的雜訊。上面的 row_var 是一個符號裡所有載波平均的雜訊；同一個符號裡，各個載波其實不一樣吵。
    // FM 的訊號弱到門檻以下時，解調出來的雜訊（喀喀聲）經過接收機的去加重，低頻比高頻大十幾 dB；訊號夠強時大致是平的。
    // 差多少、是什麼形狀，每一台接收機不一樣，所以不假設，從收到的幀自己量：
    //   每個載波收到的功率 ＝ 訊號在那個載波上的功率 ＋ 那個載波上的雜訊
    // 訊號的總量由循環字首量（cyclic_signal），照通道估計分給各個載波；扣掉之後剩下的就是雜訊。
    // 一個符號只有 49 個值，看不出形狀，所以把吵雜程度差不多的符號放在一起量（照 row_var 分成幾級；愈吵的符號形狀愈斜），
    // 同一次發射裡前幾幀量到的也算進來（Receiver::carrier_shape）。訊雜比在 kShapeMaxSnrDb 以上的符號不量、照舊。
    constexpr int kClasses = FreeTrack::kShapeClasses;
    std::vector<int> row_class(static_cast<std::size_t>(rows), -1);
    std::array<std::array<float, kCarriers>, kClasses> shape{};
    std::array<bool, kClasses> shaped{};
    if (tuning_.carrier_noise && power > 0.0) {
        for (int k = 0; k < kClasses; ++k) {
            const std::size_t i = static_cast<std::size_t>(k);
            for (double& v : free_.shape_power[i]) v *= kShapeForget;
            free_.shape_signal[i] *= kShapeForget;
            free_.shape_rows[i] *= kShapeForget;
        }
        std::array<double, kClasses> seen_rows{}, seen_noise{}, seen_var{};  // 只算這一幀的（給追蹤訊息用）
        for (int row = payload_first; row < payload_last; ++row) {
            const std::size_t r = static_cast<std::size_t>(row);
            const double var = std::max(row_var[r], floor_var);
            const double symbol_snr_db = 10.0 * std::log10(mean_power / var);
            if (symbol_snr_db >= kShapeMaxSnrDb) continue;
            const int k = std::min(static_cast<int>((kShapeMaxSnrDb - symbol_snr_db) / kShapeClassStepDb), kClasses - 1);
            row_class[r] = k;
            double signal = 0.0;
            if (!cyclic_signal(t0_ + static_cast<std::int64_t>(row) * kSymbolSamples, signal)) continue;
            signal *= static_cast<double>(kFftSize) * kFftSize;  // 換成 FFT 輸出的刻度：這個符號所有載波上訊號的功率合計
            const std::size_t i = static_cast<std::size_t>(k);
            double energy = 0.0;
            for (int c = 0; c < kCarriers; ++c) {
                const double e = std::norm(grid.at(row, c));
                free_.shape_power[i][static_cast<std::size_t>(c)] += e;
                energy += e;
            }
            free_.shape_signal[i] += signal;
            free_.shape_rows[i] += 1.0;
            seen_rows[i] += 1.0;
            seen_noise[i] += (energy - signal) / kCarriers;
            seen_var[i] += var;
        }
        std::array<double, kClasses> tilt{};
        for (int k = 0; k < kClasses; ++k) {
            const std::size_t i = static_cast<std::size_t>(k);
            shaped[i] = carrier_shape(k, base, power, shape[i], tilt[i]);
        }
        if (trace_) {
            for (int k = 0; k < kClasses; ++k) {
                const std::size_t i = static_cast<std::size_t>(k);
                if (seen_rows[i] <= 0.0) continue;
                const double top = kShapeMaxSnrDb - k * kShapeClassStepDb;
                if (k < kClasses - 1) std::fprintf(stderr, "[rx]   各載波的雜訊：訊雜比 %.0f–%.0f dB 的符號", top - kShapeClassStepDb, top);
                else std::fprintf(stderr, "[rx]   各載波的雜訊：訊雜比 %.0f dB 以下的符號", top);
                std::fprintf(stderr, " %.0f 個（累計 %.1f）", seen_rows[i], free_.shape_rows[i]);
                if (shaped[i]) std::fprintf(stderr, "，頻帶上緣比下緣 %+.1f dB", tilt[i]);
                else std::fprintf(stderr, "，量到的還不夠");
                if (seen_noise[i] > 0.0) std::fprintf(stderr, "；扣掉訊號剩下的是估計的 %+.1f dB", 10.0 * std::log10(seen_noise[i] / std::max(seen_var[i], 1e-30)));
                std::fprintf(stderr, "\n");
            }
        }
        // 自己量到的不夠的那一級，借隔壁的（先借比較吵的那一級）；都沒有就照舊
        const std::array<bool, kClasses> own = shaped;
        for (int k = 0; k < kClasses; ++k) {
            const std::size_t i = static_cast<std::size_t>(k);
            if (own[i]) continue;
            const int from = k + 1 < kClasses && own[i + 1] ? k + 1 : (k > 0 && own[i - 1] ? k - 1 : -1);
            if (from < 0) continue;
            shape[i] = shape[static_cast<std::size_t>(from)];
            shaped[i] = true;
        }
    }

    // 訊號在不在：整個酬載一次，再分成 2–4 段各看一次（對方把整幀送完了，不是送到一半就停）
    const double coherence = cyclic_coherence(t0_, payload_first, payload_last);
    const bool present = coherence > kPresenceThreshold;
    bool present_throughout = true;
    const int segments = std::clamp(mode.payload_symbols / 18, 2, 4);
    for (int k = 0; k < segments && present_throughout; ++k) {
        const int from = mode.payload_symbols * k / segments, to = mode.payload_symbols * (k + 1) / segments;
        present_throughout = cyclic_coherence(t0_, payload_first + from, payload_first + to) > kPresenceThreshold;
    }
    // 有導頻的幀在這裡還要看「幀尾附近有沒有很像 S0、還來不及判定的東西」（process_frame 的 late_candidate）：那種幀只能靠它猜
    // 這一幀有沒有在尾端被別的發射蓋掉。這種幀不必猜：結尾的參考符號對得上（tail_ok），就是對方把這一幀送到最後了。
    // 而且那個門檻（kSolidScThreshold）不高，酬載或雜訊偶爾也會超過；一超過，下一幀的前導符號如果剛好被雜訊打壞，
    // 就不會照節拍接著收，白白少一張。還沒判定的候選位置不會因此漏掉：這一幀收完之後，搜尋會從幀尾之前重新掃一次。
    const bool intact = present_throughout && tail_ok && pending_start_ < 0;
    const double top_share = payload_top_share(grid, payload_first, payload_last);
    const bool spread = top_share < kPilotTopShare;
    const bool clear = intact && spread && coherence > kFollowThreshold;
    if (trace_) {
        std::fprintf(stderr, "[rx] 幀 @%lld 模式 %d（沒有導頻）：定時修正 %+.1f；開頭的參考符號相關 %.2f、晚 %+.2f；結尾的相關 %.2f、晚 %+.2f、相位差 %+.2f；"
                             "通道是 %d 個參考符號的平均；訊雜比 %.1f dB；循環字首相干 %.2f；最強的 3 個載波佔 %.0f%%；%s；搜尋時拉平 %.2f\n",
                     static_cast<long long>(t0_), mode.id, moved_by, head.rho, tau0, end_shift.rho, tau1, dphi, references, static_cast<double>(snr_db_),
                     coherence, 100.0 * top_share, profiled ? "逐符號的雜訊" : "整幀同一個雜訊", flat_a_);
    }

    // 等化。第 row 列離開頭的參考符號 row − 1 個符號，時間差與相位照比例內插。
    std::vector<cf> cells(static_cast<std::size_t>(mode.data_cells()));
    std::vector<float> noise(cells.size());
    const double estimate_gain = 1.0 / references;  // 通道估計的誤差：等化之後多出這麼多倍的雜訊
    std::size_t n = 0;
    for (int row = payload_first; row < payload_last; ++row) {
        const double alpha = static_cast<double>(row - 1) / span;
        const double step = -kTwoPi * (tau0 + (tau1 - tau0) * alpha) / kFftSize;
        cd rot = std::polar(1.0, step * kFirstBin + phi0 + dphi * alpha);
        const cd inc = std::polar(1.0, step);
        const float var = static_cast<float>(std::max(row_var[static_cast<std::size_t>(row)], floor_var) * (1.0 + estimate_gain));
        const int k = row_class[static_cast<std::size_t>(row)];
        const float* across = k >= 0 && shaped[static_cast<std::size_t>(k)] ? shape[static_cast<std::size_t>(k)].data() : nullptr;
        for (int c = 0; c < kCarriers; ++c) {
            const cf h = base[static_cast<std::size_t>(c)] * cf(rot);
            rot *= inc;
            const float h2_power = std::max(std::norm(h), 1e-20f);
            cells[n] = grid.at(row, c) * std::conj(h) / h2_power;
            noise[n] = (across != nullptr ? var * across[c] : var) / h2_power;
            ++n;
        }
    }

    RxFrame frame;
    frame.mode_id = mode.id;
    frame.image_width = mode.image_width;
    frame.image_height = mode.image_height;
    frame.snr_db = snr_db_;
    frame.freq_offset_hz = 0.0f;
    frame.start_sample = t0_;

    // 只有確定這是一整幀才當成畫面：標頭正確；或是前導符號確認過（而且很明確，或緊接在一張好的後面）、
    // 從頭到尾都有訊號、結尾的參考符號對得上、能量也分散在整個頻帶。
    // 前導符號沒確認到的幀（confirm_next 照節拍接著收的）只有標頭正確才算數。
    const LearnedUnpacked l = learned_unpack(mode, cells, noise);
    const bool ok = l.header_ok;
    frame.header_ok = l.header_ok;
    if (l.header_ok) {
        frame.callsign = l.header.callsign_valid ? l.header.callsign : std::string();
        frame.sequence = l.header.sequence;
    }
    const bool by_preamble = !l.header_ok && confirmed_ && (previous_ok || !tentative()) && intact && spread;
    const bool whole = l.header_ok || by_preamble;
    decode_picture(mode, l, whole, frame);
    const bool emit = whole || (confirmed_ && intact && spread);
    if (trace_) std::fprintf(stderr, "[rx]   → 標頭%s、%s\n", ok ? "正確" : "沒解出來", frame.decoded ? "有畫面" : "沒有畫面");
    if (emit) {
        if (frame.decoded) ++frames_decoded_;
        else if (frame.skipped) ++frames_skipped_;
        else ++frames_failed_;
        frames_.push_back(std::move(frame));
    }
    const bool solid = whole && clear;
    last_ok_ = ok || solid;
    last_complete_ = false;
    last_clear_ = clear && (whole || confirmed_);
    follow_misses_ = 0;
    if (!ok && !present) t0_ = t0_before;  // 整幀都沒有訊號：這一幀量到的定時只是雜訊，不採用
    const double undone = t0_ == t0_before ? static_cast<double>(window_moved) : 0.0;  // 視窗移回去了：留給下一幀的通道是照移過的位置對齊的

    // 下一幀緊接在後，再加上這一幀量到的時脈漂移（不到一個取樣的部分記在 pending_shift，下一幀預測時用）
    const std::int64_t frame_start = t0_;
    const double step = slope * rows;
    free_.fraction += step;
    const std::int64_t drifted = std::llround(free_.fraction);
    free_.fraction -= static_cast<double>(drifted);
    free_.pending_shift = step - static_cast<double>(drifted) + undone;
    free_.pending_phase = dphi * rows / span;
    t0_ += mode.frame_samples() + drifted;
    if (ok || solid) {
        // 解出來的這一幀不必再搜尋；但和有導頻的幀一樣，從尾端一段一段往回看訊號到哪裡為止，
        // 對方中途停了的話，另一次發射可能就在那之後開始。
        int seen = mode.payload_symbols;
        while (seen > 0) {
            const int first = seen >= 2 * kTailSegment ? seen - kTailSegment : 0;
            if (cyclic_coherence(frame_start, payload_first + first, payload_first + seen) > kPresenceThreshold) break;
            seen = first;
        }
        const int unseen_tail = kPreambleSymbols + kVerifySymbols + 2;
        const int rescan_row = std::min(seen >= mode.payload_symbols ? mode.payload_symbols : std::max(seen - kTailSegment, 0),
                                        std::max(mode.payload_symbols - unseen_tail, 0));
        rescan_from_ = frame_start + static_cast<std::int64_t>(kPreambleSymbols + rescan_row) * kSymbolSamples;
        last_complete_ = tail_ok && seen >= mode.payload_symbols;
        if (trace_ && !last_complete_) {
            std::fprintf(stderr, "[rx]   訊號只到第 %d 個酬載符號（共 %d 個）%s：對方可能中途停了\n", seen, mode.payload_symbols,
                         tail_ok ? "" : "，結尾的參考符號也對不上");
        }
    }
    if (pending_start_ >= 0) {
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

// 把一幀收到的值解成畫面（whole：確定這是一整幀）。兩種幀共用。
void Receiver::decode_picture(const Mode& mode, const LearnedUnpacked& l, bool whole, RxFrame& frame) {
    // 沒有這個模式用的模型，或標頭說對方用的是另一份權重：解出來只會是雜訊，不交出畫面，只回報收到了一幀
    const LearnedCodec* model = models_.find(mode);
    const bool usable = model != nullptr && learned_model_matches(l, *model);
    frame.model_missing = whole && !usable;
    frame.decoded = whole && usable;
    if (frame.decoded && tuning_.skip_pictures) {
        // 設定成先不解畫面：這一幀收到的值照樣記下來，之後的幀合併時用得到
        frame.decoded = false;
        frame.skipped = true;
        const LearnedMerge* merge = tuning_.learned_merge ? find_merge(model->model_id()) : nullptr;
        if (merge != nullptr) remember(*merge, mode, l, frame);
        else earlier_.clear();
        if (trace_) std::fprintf(stderr, "[rx]   設定成先不解畫面，這一幀略過\n");
        return;
    }
    if (frame.decoded) {
        frame.rgb.resize(static_cast<std::size_t>(model->rgb_bytes()));
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
}

// 留著沒交的那一幀（Mend::held）照原樣交出去：到這時候才解成畫面。
void Receiver::release_held() {
    if (!mend_.held) return;
    mend_.held = false;
    RxFrame frame = std::move(mend_.frame);
    if (trace_) std::fprintf(stderr, "[rx] 幀 @%lld 留到現在，照原樣交出去\n", static_cast<long long>(frame.start_sample));
    decode_picture(*mend_.mode, mend_.unpacked, true, frame);
    mend_.unpacked = LearnedUnpacked{};
    if (frame.decoded) ++frames_decoded_;
    else if (frame.skipped) ++frames_skipped_;
    else ++frames_failed_;
    frames_.push_back(std::move(frame));
}

// 不再等著補救上一幀：留著沒交的照原樣交出去。
void Receiver::drop_mend() {
    release_held();
    mend_.valid = false;
}

// 回頭補救中途斷開過的一幀（RxTuning::mend_steps）。next 是剛捕獲的下一幀；補救回來、交出去了回傳 true。
//
// 斷開是什麼樣子：音訊在送到發射機的路上卡了一下（電腦一時忙不過來、音效裝置的緩衝區空了、從網路收音訊的電台等不到下一包），
// 訊號停幾毫秒到幾十毫秒，然後從停下來的地方原樣接續——之後的每個符號都晚了那麼久；或者少了一段取樣（送給電台的封包沒到、
// 哪一頭的音訊裝置丟了一段）——之後的每個符號都早了。
// 兩種情形，斷開之前的符號都在原來的位置上，之後的符號整批移了同樣的取樣數。這個數目不必猜：下一幀的前導符號出現的位置
// 和「這一幀的起點＋一幀的長度」差多少，就是它（兩邊取樣時脈的差造成的那一點先扣掉）。
//
// 做法：這一幀照原來的對時（A）與移過之後的對時（B）各解調一次。相隔 3 個符號的兩個導頻（同樣的格子）相不相干，說明
// 那兩個符號的對時對不對得上。假設在某個地方斷開：之前的符號用 A、之後的用 B，把所有的對的相關值加起來——兩個符號都在
// 前段的用 A 的、都在後段的用 B 的、一前一後的拿 A 的前一個配 B 的後一個。分界找對了，每一對都相干；找錯了，分界與真正的
// 斷點之間的符號用錯了對時，和它們配對的都加不起來。取加起來最大的分界；中間來不及對上的符號（訊號停下來的那一個，
// 少了一段時是不見的那幾個）當成沒有收到。兩段各自估通道（ChannelEstimator::estimate 的 cut_from、cut_to），
// 之後和平常的幀一樣等化、解標頭、解畫面。
//
// 什麼時候算數：接起來之後要比整幀都照原來的對時明顯更相干（kMendGain），後段的導頻也要夠清楚——不然只是下一幀晚了一點
// 才開始，這一幀本來就沒有斷。然後標頭要解得出來，或者這一幀的前導符號當時確認過（Mend::trusted）、接起來之後整幀的導頻
// 清楚而且不是單頻載波冒充的。原來標頭就解得出來的幀（留著沒交的），接起來之後標頭也要解得出來才換。
//
// 分不出來的情形：斷開的長度剛好是 3 個符號（80 毫秒）的整數倍。導頻的位置每 3 個符號重複一次，兩種對時下每一對都相干，
// 找不到分界；這種幀照原樣交（或沒有畫面）。
bool Receiver::mend_frame(const Acquisition& next) {
    if (next.mode != mend_.mode || mend_.mode == nullptr) return false;
    const Mode& mode = *mend_.mode;
    const std::int64_t length = mode.frame_samples();
    // 兩邊的取樣時脈差一點的話，下一幀的起點本來就會偏幾個取樣（最近幾幀定時修正的平均，見 process_frame）
    const double drift = trend_weight_ >= kTrendMinWeight ? trend_sum_ / trend_weight_ * static_cast<double>(length) : 0.0;
    const std::int64_t step = next.t0 - (mend_.t0 + length) - std::llround(drift);
    if (std::llabs(step) < kMendMinStep || std::llabs(step) > kMendMaxStep) return false;
    const int rows = mode.frame_symbols(), payload = mode.payload_symbols;
    const int pairs = payload - 3;  // 第 j 對是第 j 與第 j+3 個酬載符號，j = 0..pairs−1
    if (pairs < 6) return false;
    const std::int64_t ta = mend_.t0, tb = mend_.t0 + step;
    auto readable = [&](std::int64_t t0) { return t0 + kCyclicPrefix - kTrackBackoff >= base_ && t0 + length + kCyclicPrefix <= end(); };
    if (!readable(ta) || !readable(tb)) return false;

    double cfo = mend_.cfo;
    SymbolGrid a = demodulate(ta, cfo, rows), b = demodulate(tb, cfo, rows);
    // 各對的相關值：兩個符號都照 A（aa）、都照 B（bb）、前一個照 A 後一個照 B（ab）
    std::vector<cd> aa(static_cast<std::size_t>(pairs)), bb(static_cast<std::size_t>(pairs)), ab(static_cast<std::size_t>(pairs));
    for (int j = 0; j < pairs; ++j) {
        aa[static_cast<std::size_t>(j)] = pilot_pair(a, a, j);
        bb[static_cast<std::size_t>(j)] = pilot_pair(b, b, j);
        ab[static_cast<std::size_t>(j)] = pilot_pair(a, b, j);
    }
    // head[n]：兩個符號都在前 n 個酬載符號裡的那些對（j ≤ n−4），照 A；tail[n]：兩個符號都在第 n 個起的（j ≥ n），照 B
    std::vector<cd> head(static_cast<std::size_t>(payload) + 1, cd(0, 0)), tail(static_cast<std::size_t>(payload) + 1, cd(0, 0));
    for (int n = 4; n <= payload; ++n) head[static_cast<std::size_t>(n)] = head[static_cast<std::size_t>(n - 1)] + aa[static_cast<std::size_t>(n - 4)];
    for (int n = pairs - 1; n >= 0; --n) tail[static_cast<std::size_t>(n)] = tail[static_cast<std::size_t>(n + 1)] + bb[static_cast<std::size_t>(n)];
    // 分界 (ia, ib)：前 ia 個酬載符號用 A，第 ib 個起用 B，中間的 ib − ia 個當成沒有收到。跨過分界的對是 ib−3 ≤ j < ia。
    auto across = [&](int ia, int ib) {
        cd sum(0, 0);
        for (int j = std::max(ib - 3, 0); j < std::min(ia, pairs); ++j) sum += ab[static_cast<std::size_t>(j)];
        return sum;
    };
    // 少了一段時，不見的符號數是「少掉的取樣數 ÷ 符號長」無條件進位，斷在符號中間的話再多一個；訊號停下來時只有停的那一個
    const int lost_max = step < 0 ? static_cast<int>((-step + kSymbolSamples - 1) / kSymbolSamples) + 1 : 1;
    int na = 0, nb = 0;
    double best = -1.0;
    for (int ia = 0; ia + kMendMinRows <= payload; ++ia) {
        for (int ib = ia; ib <= ia + lost_max && ib + kMendMinRows <= payload; ++ib) {
            const double score = std::abs(head[static_cast<std::size_t>(ia)] + across(ia, ib) + tail[static_cast<std::size_t>(ib)]);
            if (score > best) {
                best = score;
                na = ia;
                nb = ib;
            }
        }
    }
    const cd sum_a = head[static_cast<std::size_t>(na)], sum_b = tail[static_cast<std::size_t>(nb)];
    const cd joined = sum_a + across(na, nb) + sum_b;
    const int count_a = std::max(na - 3, 0), count_b = pairs - nb;
    const int count = count_a + count_b + std::max(std::min(na, pairs) - std::max(nb - 3, 0), 0);
    const double whole_before = std::abs(head[static_cast<std::size_t>(payload)]);  // 整幀都照原來的對時
    const double gain = std::abs(joined) - whole_before;
    // 後段的導頻要夠清楚：用到後段符號的那些對（都在後段的，加上跨過分界的）平均起來的相關值。
    // 只有雜訊時，一對的相關值大小約 0.35、方向隨機，n 對平均起來約 0.35/√n；門檻取 1/√n（只有雜訊時超過的機會不到千分之一）
    const int count_after = count - count_a;
    const double after = count_after > 0 ? std::abs(joined - sum_a) / count_after : 0.0;
    const bool shifted = gain >= kMendGain && after >= std::max(kFollowThreshold, 1.0 / std::sqrt(static_cast<double>(std::max(count_after, 1))));
    if (trace_) {
        std::fprintf(stderr, "[rx] 幀 @%lld %s，下一幀的起點%s了 %lld 個取樣：前 %d 個酬載符號用原來的對時、第 %d 個起用移過的，"
                             "各對導頻的相關值加起來 %.1f（%d 對；後段平均 %.2f），整幀照原來的對時是 %.1f（%d 對）%s\n",
                     static_cast<long long>(ta), mend_.held ? "留著還沒交" : "沒有交出去", step > 0 ? "晚" : "早",
                     static_cast<long long>(std::llabs(step)), na, nb, std::abs(joined), count, after, whole_before, pairs,
                     shifted ? "" : "；不像是中途斷開，不補救");
    }
    if (!shifted) return false;

    // 殘餘頻偏：接起來之後的導頻估出來，夠大就重新解調（和 process_frame 一樣）
    const double dcfo = std::arg(joined) / 3.0 / (kTwoPi * kSymbolSamples);
    if (std::fabs(dcfo * kSampleRate) >= kCfoRedoHz) {
        cfo += dcfo;
        a = demodulate(ta, cfo, rows);
        b = demodulate(tb, cfo, rows);
    }
    const int cut_from = kPreambleSymbols + na, cut_to = kPreambleSymbols + nb;
    SymbolGrid grid(rows);
    for (int r = 0; r < rows; ++r) {
        const SymbolGrid& from = r < cut_from ? a : b;
        for (int c = 0; c < kCarriers; ++c) grid.at(r, c) = from.at(r, c);
    }
    const ChannelEstimate est = chanest_.estimate(mode, grid, cut_from, cut_to, tuning_.quiet_carriers);
    const double coherence = std::abs(joined) / std::max(count, 1);
    const PilotSpread pilots = pilot_spread(grid, mode.payload_symbols);
    const bool clear = coherence > kFollowThreshold && pilots.top_share < kPilotTopShare;

    const std::vector<CellPos> positions = data_cell_positions(mode.payload_symbols);
    std::vector<cf> cells(positions.size());
    std::vector<float> noise(positions.size());
    for (std::size_t n = 0; n < positions.size(); ++n) {
        const int row = positions[n].symbol + kPreambleSymbols;
        const int c = positions[n].bin - kFirstBin;
        if (row >= cut_from && row < cut_to) {
            cells[n] = cf(0, 0);
            noise[n] = kLostNoise;
            continue;
        }
        const cf h = est.at(row, c);
        const float h2 = std::max(std::norm(h), 1e-20f);
        cells[n] = grid.at(row, c) * std::conj(h) / h2;
        noise[n] = (tuning_.noise_map ? est.noise_at(row, c) : est.noise_var) * (1.0f + est.gain_at(row, c)) / h2;
    }

    RxFrame frame;
    frame.mode_id = mode.id;
    frame.image_width = mode.image_width;
    frame.image_height = mode.image_height;
    frame.snr_db = est.snr_db;
    frame.freq_offset_hz = static_cast<float>(cfo * kSampleRate);
    frame.start_sample = ta;
    const LearnedUnpacked l = learned_unpack(mode, cells, noise);
    frame.header_ok = l.header_ok;
    if (l.header_ok) {
        frame.callsign = l.header.callsign_valid ? l.header.callsign : std::string();
        frame.sequence = l.header.sequence;
    }
    // 留著沒交的那一幀原來標頭是對的：接起來之後標頭反而解不出來的話，不換
    const bool whole = l.header_ok || (mend_.trusted && clear && !(mend_.held && mend_.frame.header_ok));
    if (trace_) {
        std::fprintf(stderr, "[rx]   接起來之後：訊雜比 %.1f dB、導頻相干 %.2f（最強的 3 個載波佔 %.0f%%）、中間 %d 個符號當成沒有收到 → 標頭%s%s\n",
                     static_cast<double>(est.snr_db), coherence, 100.0 * pilots.top_share, nb - na, l.header_ok ? "正確" : "沒解出來",
                     whole ? "" : "，不算數");
    }
    if (!whole) return false;
    // 留著沒交的那一份不要了
    mend_.held = false;
    mend_.frame = RxFrame{};
    mend_.unpacked = LearnedUnpacked{};
    decode_picture(mode, l, true, frame);
    if (trace_) std::fprintf(stderr, "[rx]   → 補救回來：標頭%s、%s\n", l.header_ok ? "正確" : "沒解出來", frame.decoded ? "有畫面" : "沒有畫面");
    if (frame.decoded) ++frames_decoded_;
    else if (frame.skipped) ++frames_skipped_;
    else ++frames_failed_;
    frames_.push_back(std::move(frame));
    return true;
}

void Receiver::process_frame() {
    const Mode& mode = *mode_;
    const int rows = mode.frame_symbols();
    const bool previous_ok = last_ok_;
    const bool previous_clear = last_clear_;
    const std::int64_t t0_before = t0_;
    const double cfo_before = cfo_;
    // 上一幀如果還留著沒交（Mend::held），不能再等了：先照原樣交出去，交出去的順序才和收到的順序一樣
    release_held();
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

    const ChannelEstimate est = chanest_.estimate(mode, grid, -1, -1, tuning_.quiet_carriers);
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
    // 這裡原來還有一個條件：幀尾附近如果有很像 S0 的東西還來不及判定（要等後面十幾個符號到齊），這一幀就不算完整——
    // 它可能在那裡被別的發射蓋掉了。可是那個門檻（kSolidScThreshold）不高，酬載自己就常常超過：偶數格（導頻都在偶數格）
    // 相隔 128 個取樣會重複，畫面的值小的符號裡導頻佔的比例高，滑動相關就過了門檻。短波的錄音裡一成到六成五的幀都有，
    // 等於隨機否決：標頭一沒解出來，那一幀就不算數，下一幀的前導符號如果剛好衰落，也不再照節拍接著收。
    // 現在只看上面那幾段的導頻（最後一段在，就是送到最後了）。還沒判定的候選位置不會因此漏掉：收完這一幀之後，
    // 沒有確認到下一幀的前導符號時，搜尋會從幀尾之前重新掃一次。RxTuning::tail_candidate_veto 打開是原來的規則（比較用）。
    const bool late_candidate = (cand_ready_ && cand_metric_ >= kSolidScThreshold) || (peak_active_ && peak_metric_ >= kSolidScThreshold);
    const bool intact = present_throughout && pending_start_ < 0 && !(tuning_.tail_candidate_veto && late_candidate);
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
        if (!intact) {
            std::fprintf(stderr, "[rx]   不算完整：%s%s%s\n", present_throughout ? "" : "有一段沒有導頻；",
                         pending_start_ >= 0 ? "中途出現另一個前導符號；" : "",
                         tuning_.tail_candidate_veto && late_candidate ? "幀尾有還沒判定的 S0 候選" : "");
        }
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
    frame.image_width = mode.image_width;
    frame.image_height = mode.image_height;
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
    // 可疑的幀先留著不交（RxTuning::mend_steps）：標頭沒解出來、有一段沒有導頻，或導頻中間有連著幾對對不上（pilot_dip）。
    // 這一幀可能收到一半時音訊斷開過——斷開之後的符號錯位了，照這個對時解出來的畫面有一截是壞的，而導頻「還在」、
    // 標頭有重複與迴旋碼保護也常常還解得出來，所以從這一幀自己看不出來。等下一幀的位置確定：就在原來的節拍上的話
    // 照原樣交出去（confirm_next），不在的話回頭補救（begin_frame → mend_frame）。
    const bool dip = tuning_.mend_steps && whole && l.header_ok && intact && pilot_dip(grid, mode.payload_symbols);
    const bool hold = tuning_.mend_steps && whole && (!l.header_ok || !intact || dip);
    if (trace_ && dip) std::fprintf(stderr, "[rx]   導頻中間有連著三對對不上\n");
    if (!hold) decode_picture(mode, l, whole, frame);
    // 沒有畫面的幀：前導符號確認過、從頭到尾都有導頻、中途也沒有出現別的前導符號，才回報「有一幀沒解出來」。
    // 只有開頭一段有導頻的，是對方送到一半就停了，或是誤認的前導符號剛好落在別人的酬載裡。
    const bool emit = whole || (confirmed_ && intact && spread);  // 有強的載波、S1 又對不上的，多半不是一幀，不回報

    if (trace_) {
        std::fprintf(stderr, "[rx]   → 標頭%s、%s\n", ok ? "正確" : "沒解出來",
                     hold ? "有畫面（先留著，等下一幀的位置確定）" : frame.decoded ? "有畫面" : "沒有畫面");
    }
    if (emit && !hold) {
        if (frame.decoded) ++frames_decoded_;
        else if (frame.skipped) ++frames_skipped_;
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

    // 取樣時脈差多少：照節拍接下來、也解出來的幀，這一幀的定時修正了幾個取樣（mend_frame 用）
    if (from_previous_ && (ok || solid)) {
        trend_sum_ = kTrendKeep * trend_sum_ + static_cast<double>(t0_ - t0_before) / mode.frame_samples();
        trend_weight_ = kTrendKeep * trend_weight_ + 1.0;
    }
    from_previous_ = true;

    // 下一幀緊接在後
    const std::int64_t frame_start = t0_;
    last_mode_ = mode_;
    last_end_ = frame_start + mode_->frame_samples();
    // 沒有交出去的幀與留著沒交的幀都記著：下一幀的前導符號如果出現在不是整數幀的位置，它可能是中途斷開了，
    // 到時候回頭補救（mend_frame）
    mend_.valid = tuning_.mend_steps && (hold || !emit);
    mend_.held = mend_.valid && hold;
    if (mend_.valid) {
        mend_.mode = mode_;
        mend_.t0 = frame_start;
        mend_.cfo = cfo_;
        mend_.trusted = confirmed_ && (previous_ok || !tentative());
        if (hold) {
            mend_.frame = std::move(frame);
            mend_.unpacked = l;
        }
    }
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
        const PilotCheck pilots = verify_frame_start(t0_, cfo_, *mode_);
        if (trace_) std::fprintf(stderr, "[rx]   上一幀不完整，導頻驗證：相關 %.2f、頻偏修正 %+.2f Hz\n", pilots.rho, pilots.cfo_delta * kSampleRate);
        if (pilots.rho >= (mode_->scattered_pilots ? tuning_.verify_pilots : tuning_.verify_cyclic)) {
            cfo_ += pilots.cfo_delta;
        } else {
            confirmed = false;
            mode_ = previous_mode;
        }
    }

    if (confirmed) {
        drop_mend();  // 下一幀就在原來的節拍上：上一幀沒有斷開過。留著沒交的照原樣交出去；沒解出來的就是沒解出來
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
