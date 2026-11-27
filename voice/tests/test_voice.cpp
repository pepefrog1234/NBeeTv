// 語音模組（voice/include/nbtv/voice.h）。沒有把編解碼器編進來時只檢查空殼；
// 有的話用 rade_c 附的一小段語音走一遍：語音 → 訊號 → 語音，單獨走，以及和畫面一起經過合成與分離（duplex.hpp）。
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
// 兩邊的時間差要找：接收端同步之前的那一小段（約 0.3 秒）沒有語音出來，所以解出來的第一個取樣對應的是原來的 0.3 秒左右。
double loudness_match(std::span<const float> sent, std::span<const float> got) {
    const std::vector<double> a = loudness(sent), b = loudness(got);
    double best = -1.0;
    for (int skip = 0; skip <= 40; ++skip) {  // got 的第 0 格對 sent 的第 skip 格，最多看到 0.8 秒
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

void test_codec() {
    const std::vector<float> speech = sample_speech();
    if (speech.empty()) return;
    const double seconds = static_cast<double>(speech.size()) / NBTV_VOICE_SPEECH_RATE;
    CHECK(std::string(nbtv_voice_description()).find("RADE") != std::string::npos);

    nbtv_voice_tx* tx = nbtv_voice_tx_create();
    nbtv_voice_rx* rx = nbtv_voice_rx_create();
    CHECK(tx != nullptr && rx != nullptr);
    if (tx == nullptr || rx == nullptr) return;

    // 語音 → 訊號：長度和語音一樣（多一個「結束」的標記），大小是 RADE V2 平常的大小
    const std::vector<float> iq = encode(tx, speech, 441);
    const double signal_seconds = static_cast<double>(iq.size() / 2) / NBTV_VOICE_SIGNAL_RATE;
    CHECK(signal_seconds > seconds && signal_seconds < seconds + 0.4);
    const double level = rms(iq) * std::sqrt(2.0);  // 複數的均方根
    CHECK_NEAR(level, kVoiceNominalRms, 0.05);
    float peak = 0.0f;
    for (std::size_t i = 0; i < iq.size(); i += 2) peak = std::max(peak, std::hypot(iq[i], iq[i + 1]));
    CHECK(peak < 1.2f);
    // 結束之後再給語音不會有東西出來；reset 之後重來，結果完全相同（一次給完也一樣）
    nbtv_voice_tx_push(tx, speech.data(), 4800);
    CHECK_EQ(nbtv_voice_tx_available(tx), 0);
    nbtv_voice_tx_reset(tx);
    CHECK(encode(tx, speech, speech.size()) == iq);
    // 還沒結束時，訊號比語音少不到一段（40 ms）
    nbtv_voice_tx_reset(tx);
    const std::vector<float> open_ended = encode(tx, speech, 960, false);
    CHECK(open_ended.size() <= iq.size() && iq.size() - open_ended.size() < 2 * 2000);
    CHECK(std::equal(open_ended.begin(), open_ended.end(), iq.begin()));

    // 訊號 → 語音。後面補一秒靜音，讓最後一段也出來
    std::vector<float> padded = iq;
    padded.resize(padded.size() + 2 * NBTV_VOICE_SIGNAL_RATE, 0.0f);
    const Decoded direct = decode(rx, padded, 160);
    const double out_seconds = static_cast<double>(direct.speech.size()) / NBTV_VOICE_SPEECH_RATE;
    CHECK(out_seconds > seconds - 0.6 && out_seconds < seconds + 0.6);
    CHECK(direct.synced_share > 0.8 * seconds / (seconds + 1.0));
    CHECK(direct.snr_db > 15.0);
    CHECK_EQ(direct.last.overs_ended, 1);
    const double match = loudness_match(speech, direct.speech);
    CHECK(match > 0.8);
    CHECK(rms(direct.speech) > 0.3 * rms(speech) && rms(direct.speech) < 3.0 * rms(speech));
    std::printf("  語音 %.1f 秒 → 訊號 %.2f 秒（均方根 %.3f、峰值 %.2f）→ 語音 %.2f 秒；同步 %.0f%%、訊雜比 %.1f dB；音量起伏和原來的相關 %.2f\n",
                seconds, signal_seconds, level, static_cast<double>(peak), out_seconds, 100.0 * direct.synced_share, direct.snr_db, match);
    // 分的大小不同，解出來的完全相同；reset 之後也一樣
    nbtv_voice_rx_reset(rx);
    CHECK(decode(rx, padded, 1).speech == direct.speech);
    nbtv_voice_rx_reset(rx);
    CHECK(decode(rx, padded, 3331).speech == direct.speech);

    // 只有雜訊：不同步，也沒有語音出來
    nbtv_voice_rx_reset(rx);
    Rng rng(5);
    std::vector<float> noise(2 * 8000 * 6);
    for (float& v : noise) v = static_cast<float>(0.3 * rng.gauss());
    const Decoded from_noise = decode(rx, noise, 160);
    CHECK(from_noise.synced_share < 0.05);
    CHECK(from_noise.speech.size() < 4800);

    // 和畫面一起：經過合成（DuplexTx）與分離（DuplexRx），兩種版面
    if (link_util::have_models()) {
        const Mode& mode = *find_mode(3);
        const std::vector<float> picture = link_util::burst(mode, 6, "N0CALL");
        for (DuplexLayout layout : {DuplexLayout::Standard, DuplexLayout::Narrow}) {
            DuplexTx mux(layout);
            mux.push_picture(picture);
            std::vector<cf> voice(iq.size() / 2);
            for (std::size_t i = 0; i < voice.size(); ++i) voice[i] = cf(iq[2 * i], iq[2 * i + 1]);
            mux.push_voice(voice);
            std::vector<float> air(std::max(mux.pending_picture(), mux.pending_voice()) + kDuplexRate);
            mux.read(air);
            DuplexRx demux(layout);
            demux.push(air);
            std::vector<float> picture_out(demux.available_picture());
            demux.read_picture(picture_out);
            std::vector<cf> voice_out(demux.available_voice());
            demux.read_voice(voice_out);
            std::vector<float> iq_out(2 * voice_out.size());
            for (std::size_t i = 0; i < voice_out.size(); ++i) {
                iq_out[2 * i] = voice_out[i].real();
                iq_out[2 * i + 1] = voice_out[i].imag();
            }
            nbtv_voice_rx_reset(rx);
            const Decoded shared = decode(rx, iq_out, 160);
            const double shared_match = loudness_match(speech, shared.speech);
            CHECK(shared.synced_share > 0.7 * seconds / (seconds + 1.0));
            CHECK(shared.snr_db > 10.0);
            CHECK(shared_match > 0.8);
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

}  // namespace

int main() {
    test_null_safety();
    if (!nbtv_voice_available()) {
        CHECK(nbtv_voice_tx_create() == nullptr);
        CHECK(nbtv_voice_rx_create() == nullptr);
        CHECK(std::string(nbtv_voice_description()).empty());
        std::printf("  這份程式沒有把語音的編解碼器編進來（-DNBTV_VOICE=ON），只檢查空殼\n");
        return check::finish("voice");
    }
    test_codec();
    return check::finish("voice");
}
