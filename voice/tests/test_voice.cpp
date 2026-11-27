// 語音模組（voice/include/nbtv/voice.h）。沒有把編解碼器編進來時只檢查空殼；
// 有的話用 rade_c 附的一小段語音走一遍：語音 → 訊號 → 語音，單獨走，以及和畫面一起經過合成與分離（duplex.hpp）。
// 兩版的 RADE 都走：V2 配分頻的兩種版面，V1 配「疊加」。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "check.hpp"
#include "duplex.hpp"
#include "link_util.hpp"
#include "nbtv/voice.h"
#include "wav.hpp"

using namespace nbtv;

namespace {

double rms(std::span<const float> x) { return std::sqrt(link_util::mean_power(x)); }

// 空指標、沒有東西可取：都不會出事
void test_null_safety() {
    nbtv_voice_tx_destroy(nullptr);
    nbtv_voice_rx_destroy(nullptr);
    nbtv_voice_tx_reset(nullptr);
    nbtv_voice_rx_reset(nullptr);
    nbtv_voice_tx_push(nullptr, nullptr, 0);
    nbtv_voice_tx_finish(nullptr);
    nbtv_voice_rx_push(nullptr, nullptr, 0);
    float out[4] = {1, 1, 1, 1};
    CHECK_EQ(nbtv_voice_tx_available(nullptr), 0);
    CHECK_EQ(nbtv_voice_tx_read(nullptr, out, 2), 0);
    CHECK_EQ(nbtv_voice_rx_available(nullptr), 0);
    CHECK_EQ(nbtv_voice_rx_read(nullptr, out, 4), 0);
    nbtv_voice_rx_status status{1, 1.0f, 1.0f, 1};
    nbtv_voice_rx_get_status(nullptr, &status);
    CHECK(status.synced == 0 && status.overs_ended == 0);
    nbtv_voice_rx_get_status(nullptr, nullptr);
}

// 測試用的語音（48 kHz）。讀不到時是空的。
std::vector<float> sample_speech() {
    std::vector<float> speech;
    int rate = 0;
    const std::string path = NBTV_VOICE_SAMPLE;
    if (path.empty() || !tools::read_wav(path, speech, rate) || rate != 16000) {
        std::printf("  讀不到測試用的語音 %s\n", path.c_str());
        return {};
    }
    std::vector<float> up;
    duplex_detail::Resampler(3, 1, 16000.0, 7400.0, 8600.0).process(speech, up);
    return up;
}

// 語音 → 訊號。chunk 是一次給幾個取樣。
std::vector<float> encode(nbtv_voice_tx* tx, std::span<const float> speech, std::size_t chunk, bool finish = true) {
    std::vector<float> iq, block(2 * 1000);
    auto drain = [&] {
        while (nbtv_voice_tx_available(tx) > 0) {
            const int n = nbtv_voice_tx_read(tx, block.data(), 1000);
            iq.insert(iq.end(), block.begin(), block.begin() + 2 * n);
        }
    };
    for (std::size_t pos = 0; pos < speech.size(); pos += chunk) {
        nbtv_voice_tx_push(tx, speech.data() + pos, static_cast<int>(std::min(chunk, speech.size() - pos)));
        drain();
    }
    if (finish) nbtv_voice_tx_finish(tx);
    drain();
    return iq;
}

struct Decoded {
    std::vector<float> speech;
    double synced_share = 0.0;  // 推訊號的過程中，有幾成的時間是同步的
    double snr_db = 0.0;        // 同步時的平均
    nbtv_voice_rx_status last{};
};

// 訊號 → 語音。iq 是 I、Q 交錯；chunk 是一次給幾個複數。
Decoded decode(nbtv_voice_rx* rx, std::span<const float> iq, std::size_t chunk) {
    Decoded out;
    std::vector<float> block(4800);
    long steps = 0, synced = 0;
    for (std::size_t pos = 0; pos < iq.size() / 2; pos += chunk) {
        nbtv_voice_rx_push(rx, iq.data() + 2 * pos, static_cast<int>(std::min(chunk, iq.size() / 2 - pos)));
        while (nbtv_voice_rx_available(rx) > 0) {
            const int n = nbtv_voice_rx_read(rx, block.data(), static_cast<int>(block.size()));
            out.speech.insert(out.speech.end(), block.begin(), block.begin() + n);
        }
        nbtv_voice_rx_get_status(rx, &out.last);
        ++steps;
        if (out.last.synced) {
            ++synced;
            out.snr_db += out.last.snr_db;
        }
    }
    if (synced > 0) out.snr_db /= static_cast<double>(synced);
    out.synced_share = steps > 0 ? static_cast<double>(synced) / static_cast<double>(steps) : 0.0;
    return out;
}

// 每 20 ms 的音量（dB）
std::vector<double> loudness(std::span<const float> speech) {
    const std::size_t frame = NBTV_VOICE_SPEECH_RATE / 50;
    std::vector<double> out;
    for (std::size_t pos = 0; pos + frame <= speech.size(); pos += frame) {
        out.push_back(10.0 * std::log10(link_util::mean_power(speech.subspan(pos, frame)) + 1e-7));
    }
    return out;
}

// 解出來的語音和原來的像不像：兩邊每 20 ms 的音量起伏的相關係數（合成的語音波形不會一樣，但哪裡大聲、哪裡安靜要一樣）。
// 兩邊的時間差要找：接收端同步之前的那一小段（V2 約 0.3 秒，V1 約 0.7 秒）沒有語音出來，所以解出來的第一個取樣
// 對應的是原來的 0.3 秒或 0.7 秒左右。
double loudness_match(std::span<const float> sent, std::span<const float> got) {
    const std::vector<double> a = loudness(sent), b = loudness(got);
    double best = -1.0;
    for (int skip = 0; skip <= 75; ++skip) {  // got 的第 0 格對 sent 的第 skip 格，最多看到 1.5 秒
        const std::size_t first = static_cast<std::size_t>(skip);
        if (a.size() < first + 50) break;
        const std::size_t n = std::min(a.size() - first, b.size());
        if (n < 50) break;
        double ma = 0, mb = 0;
        for (std::size_t i = 0; i < n; ++i) {
            ma += a[i + first];
            mb += b[i];
        }
        ma /= static_cast<double>(n);
        mb /= static_cast<double>(n);
        double sab = 0, saa = 0, sbb = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double da = a[i + first] - ma, db = b[i] - mb;
            sab += da * db;
            saa += da * da;
            sbb += db * db;
        }
        if (saa > 0 && sbb > 0) best = std::max(best, sab / std::sqrt(saa * sbb));
    }
    return best;
}

// 一段訊號（I、Q 交錯）→ 複數
std::vector<cf> to_complex(std::span<const float> iq) {
    std::vector<cf> out(iq.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = cf(iq[2 * i], iq[2 * i + 1]);
    return out;
}

std::vector<float> to_interleaved(std::span<const cf> x) {
    std::vector<float> out(2 * x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        out[2 * i] = x[i].real();
        out[2 * i + 1] = x[i].imag();
    }
    return out;
}

// 兩版不一樣的數字
struct CodecFacts {
    int codec;
    DuplexLayout layout;     // 用這一版的版面（其中一種）：訊號交進去時的大小照它算
    int gap;                 // 中途停的單位（8 kHz 的複數個數）
    double finish_seconds;   // 結束時最多多出多少：還沒湊滿的那一段補滿，加上「結束」的標記
    double sync_seconds;     // 接收端從訊號開始到有語音出來，最多這麼久
    std::vector<int> pauses; // 測「停一下再接回去」時停幾個單位
};

const CodecFacts kV2{NBTV_VOICE_RADE_V2, DuplexLayout::Standard, 160, 0.25, 0.6, {1, 3, 8}};
// V1 的一段是 120 ms、「結束」的標記 144 ms；接收端要連續看到四個數據幀的導頻才算同步（約 0.6 秒）。
// 停太久（接近一秒）它會因為那一秒裡解出來的輔助位元錯太多而放棄，所以只停到三個單位（0.36 秒）
const CodecFacts kV1{NBTV_VOICE_RADE_V1, DuplexLayout::Overlay, 960, 0.27, 1.0, {1, 2, 3}};

void test_codec(const CodecFacts& facts) {
    const std::vector<float> speech = sample_speech();
    if (speech.empty()) return;
    const double seconds = static_cast<double>(speech.size()) / NBTV_VOICE_SPEECH_RATE;
    const std::string name = nbtv_voice_codec_name(facts.codec);
    CHECK(name.find("RADE") != std::string::npos);
    CHECK_EQ(nbtv_voice_gap_samples(facts.codec), facts.gap);
    CHECK_EQ(duplex_voice_codec(facts.layout), facts.codec);

    nbtv_voice_tx* tx = nbtv_voice_tx_create(facts.codec);
    nbtv_voice_rx* rx = nbtv_voice_rx_create(facts.codec);
    CHECK(tx != nullptr && rx != nullptr);
    if (tx == nullptr || rx == nullptr) return;

    // 語音 → 訊號：長度和語音一樣（多一個「結束」的標記），大小是這一版 RADE 平常的大小
    const std::vector<float> iq = encode(tx, speech, 441);
    const double signal_seconds = static_cast<double>(iq.size() / 2) / NBTV_VOICE_SIGNAL_RATE;
    CHECK(signal_seconds > seconds && signal_seconds < seconds + 0.4);
    const double level = rms(iq) * std::sqrt(2.0);  // 複數的均方根
    CHECK_NEAR(level, duplex_voice_nominal_rms(facts.layout), 0.05);
    float peak = 0.0f;
    for (std::size_t i = 0; i < iq.size(); i += 2) peak = std::max(peak, std::hypot(iq[i], iq[i + 1]));
    CHECK(peak < 1.2f);
    // 結束之後再給語音不會有東西出來；reset 之後重來，結果完全相同（一次給完也一樣）
    nbtv_voice_tx_push(tx, speech.data(), 4800);
    CHECK_EQ(nbtv_voice_tx_available(tx), 0);
    nbtv_voice_tx_reset(tx);
    CHECK(encode(tx, speech, speech.size()) == iq);
    // 還沒結束時，訊號只比結束了的少最後那一點（還沒湊滿的一段與「結束」的標記）
    nbtv_voice_tx_reset(tx);
    const std::vector<float> open_ended = encode(tx, speech, 960, false);
    CHECK(open_ended.size() <= iq.size() &&
          static_cast<double>(iq.size() - open_ended.size()) < 2.0 * facts.finish_seconds * NBTV_VOICE_SIGNAL_RATE);
    CHECK(std::equal(open_ended.begin(), open_ended.end(), iq.begin()));

    // 訊號 → 語音。後面補一秒靜音，讓最後一段也出來
    std::vector<float> padded = iq;
    padded.resize(padded.size() + 2 * NBTV_VOICE_SIGNAL_RATE, 0.0f);
    const Decoded direct = decode(rx, padded, 160);
    const double out_seconds = static_cast<double>(direct.speech.size()) / NBTV_VOICE_SPEECH_RATE;
    CHECK(out_seconds > seconds - facts.sync_seconds && out_seconds < seconds + 0.6);
    CHECK(direct.synced_share > 0.8 * (seconds - facts.sync_seconds) / (seconds + 1.0));
    CHECK(direct.snr_db > 15.0);
    CHECK_EQ(direct.last.overs_ended, 1);
    const double match = loudness_match(speech, direct.speech);
    CHECK(match > 0.8);
    CHECK(rms(direct.speech) > 0.3 * rms(speech) && rms(direct.speech) < 3.0 * rms(speech));
    std::printf("  %s：語音 %.1f 秒 → 訊號 %.2f 秒（均方根 %.3f、峰值 %.2f）→ 語音 %.2f 秒；同步 %.0f%%、訊雜比 %.1f dB；音量起伏和原來的相關 %.2f\n",
                name.c_str(), seconds, signal_seconds, level, static_cast<double>(peak), out_seconds, 100.0 * direct.synced_share, direct.snr_db, match);
    // 分的大小不同，解出來的完全相同；reset 之後也一樣
    nbtv_voice_rx_reset(rx);
    CHECK(decode(rx, padded, 1).speech == direct.speech);
    nbtv_voice_rx_reset(rx);
    CHECK(decode(rx, padded, 3331).speech == direct.speech);

    // 訊號送到一半停一下再接著送（電腦一時忙不過來）：停的長度是那個單位（V2 一個符號、V1 一個數據幀）的整數倍的話，
    // 接收端不必重新同步——就算停在結尾前不到半秒，「結束」的標記也收得到。外殼的音訊輸出靠的就是這一點：
    // 語音的佇列空掉之後，等到停的長度是它的整數倍才接回去（nbtv_voice_gap_samples 的說明）。
    const int symbol = nbtv_voice_gap_samples(facts.codec);
    CHECK(symbol > 0);
    for (int symbols : facts.pauses) {
        std::vector<float> paused = iq;
        const std::size_t at = 2 * (iq.size() / 2 - static_cast<std::size_t>(0.3 * NBTV_VOICE_SIGNAL_RATE));
        paused.insert(paused.begin() + static_cast<std::ptrdiff_t>(at), 2 * static_cast<std::size_t>(symbols * symbol), 0.0f);
        paused.resize(paused.size() + 2 * NBTV_VOICE_SIGNAL_RATE, 0.0f);
        nbtv_voice_rx_reset(rx);
        const Decoded got = decode(rx, paused, 160);
        CHECK_EQ(got.last.overs_ended, 1);
        // 語音也一路解到最後：只少同步之前的那一小段
        CHECK(static_cast<double>(got.speech.size()) / NBTV_VOICE_SPEECH_RATE > seconds - facts.sync_seconds);
    }
    {
        // 反過來，停的長度不是整數倍（半個單位）：接收端要重新同步，停在結尾前一秒半的話「結束」的標記多半就收不到了。
        // 這是外殼要湊整數倍的理由。只印出來看，不檢查：重新同步要多久是編解碼器自己的事
        std::vector<float> paused = iq;
        const std::size_t at = 2 * (iq.size() / 2 - static_cast<std::size_t>(1.5 * NBTV_VOICE_SIGNAL_RATE));
        paused.insert(paused.begin() + static_cast<std::ptrdiff_t>(at), 2 * static_cast<std::size_t>(symbol / 2), 0.0f);
        paused.resize(paused.size() + 2 * NBTV_VOICE_SIGNAL_RATE, 0.0f);
        nbtv_voice_rx_reset(rx);
        std::printf("  %s：在結尾前 1.5 秒停半個單位（%d 個取樣，不是整數倍）：「結束」的標記收到 %d 次（停整數倍時 1 次）\n", name.c_str(),
                    symbol / 2, decode(rx, paused, 160).last.overs_ended);
    }

    // 只有雜訊：不同步，也沒有語音出來
    nbtv_voice_rx_reset(rx);
    Rng rng(5);
    std::vector<float> noise(2 * 8000 * 6);
    for (float& v : noise) v = static_cast<float>(0.3 * rng.gauss());
    const Decoded from_noise = decode(rx, noise, 160);
    CHECK(from_noise.synced_share < 0.05);
    CHECK(from_noise.speech.size() < 4800);

    // 和畫面一起：經過合成（DuplexTx）與分離（DuplexRx），分頻的兩種版面（疊加的在 test_overlay）
    if (facts.codec == NBTV_VOICE_RADE_V2 && link_util::have_models()) {
        const Mode& mode = *find_mode(3);
        const std::vector<float> picture = link_util::burst(mode, 6, "N0CALL");
        for (DuplexLayout layout : {DuplexLayout::Standard, DuplexLayout::Narrow}) {
            DuplexTx mux(layout);
            mux.push_picture(picture);
            const std::vector<cf> voice = to_complex(iq);
            mux.push_voice(voice);
            std::vector<float> air(std::max(mux.pending_picture(), mux.pending_voice()) + kDuplexRate);
            mux.read(air);
            DuplexRx demux(layout);
            demux.push(air);
            std::vector<float> picture_out(demux.available_picture());
            demux.read_picture(picture_out);
            std::vector<cf> voice_out(demux.available_voice());
            demux.read_voice(voice_out);
            const std::vector<float> iq_out = to_interleaved(voice_out);
            nbtv_voice_rx_reset(rx);
            const Decoded shared = decode(rx, iq_out, 160);
            const double shared_match = loudness_match(speech, shared.speech);
            CHECK(shared.synced_share > 0.7 * seconds / (seconds + 1.0));
            CHECK(shared.snr_db > 10.0);
            CHECK(shared_match > 0.8);
            CHECK_EQ(shared.last.overs_ended, 1);
            // 合成之後才停一下也一樣（外殼就是在這裡停的；這時候一個符號是 48 kHz 的 960 個取樣）：
            // 在語音結束前 0.3 秒停三個符號，「結束」的標記照樣收到
            {
                const std::size_t ratio = static_cast<std::size_t>(kDuplexRate / NBTV_VOICE_SIGNAL_RATE);
                const std::size_t voice_end = voice.size() * ratio;
                std::vector<float> paused_air = air;
                paused_air.insert(paused_air.begin() + static_cast<std::ptrdiff_t>(voice_end - static_cast<std::size_t>(0.3 * kDuplexRate)),
                                  3 * static_cast<std::size_t>(symbol) * ratio, 0.0f);
                DuplexRx split(layout);
                split.push(paused_air);
                std::vector<cf> paused_voice(split.available_voice());
                split.read_voice(paused_voice);
                nbtv_voice_rx_reset(rx);
                CHECK_EQ(decode(rx, to_interleaved(paused_voice), 160).last.overs_ended, 1);
            }
            // 畫面也都收得到
            Receiver receiver;
            link_util::attach_models(receiver);
            receiver.tuning().band_shift_hz = duplex_band_shift_hz(layout);
            int good = 0;
            for (const RxFrame& f : link_util::run_receiver(receiver, picture_out)) good += (f.decoded && f.header_ok && f.mode_id == mode.id) ? 1 : 0;
            CHECK_EQ(good, 6);
            std::printf("  %s、和畫面一起：語音同步 %.0f%%、訊雜比 %.1f dB、音量起伏的相關 %.2f；畫面 %d/6 幀\n",
                        layout == DuplexLayout::Narrow ? "窄版面" : "標準版面", 100.0 * shared.synced_share, shared.snr_db, shared_match, good);
        }
    }
    nbtv_voice_tx_destroy(tx);
    nbtv_voice_rx_destroy(rx);
}

// 「疊加」的版面：RADE V1 的語音原樣加在畫面上（DuplexLayout::Overlay）。
void test_overlay() {
    const std::vector<float> speech = sample_speech();
    if (speech.empty()) return;
    const double seconds = static_cast<double>(speech.size()) / NBTV_VOICE_SPEECH_RATE;
    const DuplexLayout layout = DuplexLayout::Overlay;
    nbtv_voice_tx* tx = nbtv_voice_tx_create(NBTV_VOICE_RADE_V1);
    nbtv_voice_rx* rx = nbtv_voice_rx_create(NBTV_VOICE_RADE_V1);
    CHECK(tx != nullptr && rx != nullptr);
    if (tx == nullptr || rx == nullptr) return;
    const std::vector<float> iq = encode(tx, speech, 960);
    const std::vector<cf> voice = to_complex(iq);

    // 合成、分離之後語音那一路交出來的訊號，交給 V1 的解碼器
    auto through = [&](std::span<const float> air) {
        DuplexRx demux(layout);
        demux.enable(false, true);
        demux.push(air);
        std::vector<cf> voice_out(demux.available_voice());
        demux.read_voice(voice_out);
        nbtv_voice_rx_reset(rx);
        return decode(rx, to_interleaved(voice_out), 160);
    };
    auto mix = [&](std::span<const float> picture, float voice_gain_db) {
        DuplexTx mux(layout);
        mux.set_voice_gain_db(voice_gain_db);
        if (!picture.empty()) mux.push_picture(picture);
        mux.push_voice(voice);
        std::vector<float> air(std::max(mux.pending_picture(), mux.pending_voice()) + kDuplexRate);
        mux.read(air);
        return air;
    };

    // 只有語音：過了這一層（取實部、換成 48 kHz、換回 8 kHz、變回解析訊號）和直接解一樣好
    const std::vector<float> voice_air = mix({}, 0.0f);
    const Decoded alone = through(voice_air);
    const double alone_match = loudness_match(speech, alone.speech);
    CHECK(alone.synced_share > 0.8 * (seconds - kV1.sync_seconds) / (seconds + 1.0));
    CHECK(alone.snr_db > 15.0);
    CHECK(alone_match > 0.8);
    CHECK_EQ(alone.last.overs_ended, 1);
    std::printf("  疊加、只有語音：同步 %.0f%%、訊雜比 %.1f dB、音量起伏的相關 %.2f\n", 100.0 * alone.synced_share, alone.snr_db, alone_match);
    // 送出去的就是 RADE V1 訊號的實部，沒有濾過也沒有移過：只用 FreeDV 的人收到的是它認得的波形。
    // 拿原來的訊號的實部比（差一個固定的延遲與增益；延遲不是 48 kHz 取樣的整數倍，所以相關係數到不了 1）
    {
        const std::size_t ratio = static_cast<std::size_t>(kDuplexRate / NBTV_VOICE_SIGNAL_RATE);
        double best = 0.0;
        for (std::size_t delay = 0; delay < 20 * ratio; ++delay) {
            double ab = 0.0, aa = 0.0, bb = 0.0;
            for (std::size_t n = 2000; n < 10000; ++n) {
                const double a = voice[n].real(), b = voice_air[n * ratio + delay];
                ab += a * b;
                aa += a * a;
                bb += b * b;
            }
            best = std::max(best, ab / std::sqrt(aa * bb));
        }
        std::printf("  疊加：送出去的語音和 RADE V1 給的訊號的實部，相關係數 %.4f\n", best);
        CHECK(best > 0.98);
    }

    if (!link_util::have_models()) {
        nbtv_voice_tx_destroy(tx);
        nbtv_voice_rx_destroy(rx);
        return;
    }
    // 和畫面一起、兩邊功率相同（預設）：兩邊都解得出來，但各自都把對方當成雜訊。
    // 模式 13（每張 2.72 秒）四幀、模式 14（每張 0.67 秒）十五幀，都是十秒上下，和語音差不多長
    struct Case {
        int mode_id, frames, min_pictures;
    };
    for (const Case& c : {Case{13, 4, 3}, Case{14, 15, 12}}) {
        const Mode& mode = *find_mode(c.mode_id);
        const std::vector<float> picture = link_util::burst(mode, c.frames, "N0CALL");
        const std::vector<float> air = mix(picture, 0.0f);
        float peak = 0.0f;
        for (float v : air) peak = std::max(peak, std::fabs(v));
        CHECK(peak < 1.0f);
        const Decoded shared = through(air);
        const double shared_match = loudness_match(speech, shared.speech);
        // 語音：大部分的時間同步著，解出來的音量起伏還認得出是同一段話
        CHECK(shared.synced_share > 0.6 * (seconds - kV1.sync_seconds) / (seconds + 1.0));
        CHECK(shared_match > 0.6);
        // 畫面：平常的接收機直接收合成的音訊（換成 12 kHz），不必分
        DuplexRx demux(layout);
        demux.enable(true, false);
        demux.push(air);
        std::vector<float> picture_out(demux.available_picture());
        demux.read_picture(picture_out);
        Receiver receiver;
        link_util::attach_models(receiver);
        int pictures = 0, headers = 0;
        for (const RxFrame& f : link_util::run_receiver(receiver, picture_out)) {
            if (!f.decoded || f.mode_id != mode.id) continue;
            ++pictures;
            headers += f.header_ok ? 1 : 0;
        }
        CHECK(pictures >= c.min_pictures);
        std::printf("  疊加、模式 %d 和語音一起（功率相同，合成後峰值 %.2f）：畫面 %d/%d 張（標頭正確 %d）；語音同步 %.0f%%、"
                    "量到的訊雜比 %.1f dB、音量起伏的相關 %.2f、「結束」的標記 %d 次\n",
                    mode.id, static_cast<double>(peak), pictures, c.frames, headers, 100.0 * shared.synced_share, shared.snr_db, shared_match,
                    shared.last.overs_ended);
    }
    nbtv_voice_tx_destroy(tx);
    nbtv_voice_rx_destroy(rx);
}

}  // namespace

int main() {
    test_null_safety();
    // 不認得的版本：什麼都沒有
    CHECK(nbtv_voice_tx_create(0) == nullptr);
    CHECK(nbtv_voice_rx_create(3) == nullptr);
    CHECK(std::string(nbtv_voice_codec_name(0)).empty());
    CHECK_EQ(nbtv_voice_gap_samples(0), 0);
    if (!nbtv_voice_available()) {
        for (int codec : {NBTV_VOICE_RADE_V1, NBTV_VOICE_RADE_V2}) {
            CHECK(nbtv_voice_tx_create(codec) == nullptr);
            CHECK(nbtv_voice_rx_create(codec) == nullptr);
            CHECK(std::string(nbtv_voice_codec_name(codec)).empty());
            CHECK_EQ(nbtv_voice_gap_samples(codec), 0);
        }
        CHECK(std::string(nbtv_voice_description()).empty());
        std::printf("  這份程式沒有把語音的編解碼器編進來（-DNBTV_VOICE=ON），只檢查空殼\n");
        return check::finish("voice");
    }
    CHECK(std::string(nbtv_voice_description()).find("RADE V1 + V2") != std::string::npos);
    test_codec(kV2);
    test_codec(kV1);
    test_overlay();
    return check::finish("voice");
}
