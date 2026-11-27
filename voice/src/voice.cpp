// include/nbtv/voice.h 的實作：語音的編解碼器（rade_glue.c 接的 RADE，V1 與 V2）前後的事——48 kHz 與 16 kHz 之間的重取樣、
// 把任意長度的輸入切成編解碼器要的大小、把輸出排好等人來取。
// 建置時沒有打開語音（NBTV_VOICE_RADE 是 0）的話，這裡只剩下「沒有」的那幾個函式。
#include "nbtv/voice.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#if NBTV_VOICE_RADE

#include "dsp.hpp"
#include "duplex.hpp"
#include "rade_glue.h"

namespace {

constexpr int kCodecRate = 16000;  // 編解碼器用的語音取樣率
constexpr int kSpeechRatio = NBTV_VOICE_SPEECH_RATE / kCodecRate;
// 48 kHz 與 16 kHz 之間：留到 7.4 kHz，8.6 kHz 以上（會疊回 7.4 kHz 以下的部分）壓乾淨
constexpr double kSpeechPass = 7400.0, kSpeechStop = 8600.0;

// 等著被取走的資料：前面已經取走的夠多了就丟掉
void drop_consumed(std::vector<float>& buffer, std::size_t& read) {
    if (read >= 16384 && read * 2 >= buffer.size()) {
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
        read = 0;
    }
}

}  // namespace

struct nbtv_voice_tx {
    nbtv_rade_tx* codec = nullptr;
    nbtv::duplex_detail::Resampler down{1, kSpeechRatio, NBTV_VOICE_SPEECH_RATE, kSpeechPass, kSpeechStop};
    std::vector<float> clean;
    std::vector<float> speech;  // 16 kHz、還沒湊滿一個語音幀的
    std::vector<float> block;   // 編解碼器一次寫出的訊號
    std::vector<float> iq;      // 等著被取走的訊號（I、Q 交錯）
    std::size_t iq_read = 0;
    bool finished = false;
};

struct nbtv_voice_rx {
    nbtv_rade_rx* codec = nullptr;
    nbtv::duplex_detail::Resampler up{kSpeechRatio, 1, kCodecRate, kSpeechPass, kSpeechStop};
    std::vector<float> iq;       // 還沒交給編解碼器的訊號（I、Q 交錯）
    std::vector<float> decoded;  // 編解碼器一次寫出的語音（16 kHz）
    std::vector<float> speech;   // 等著被取走的語音（48 kHz）
    std::size_t speech_read = 0;
    int overs_ended = 0;
};

static_assert(NBTV_VOICE_RADE_V1 == NBTV_RADE_V1 && NBTV_VOICE_RADE_V2 == NBTV_RADE_V2);
static_assert(NBTV_VOICE_RADE_V1 == nbtv::duplex_voice_codec(nbtv::DuplexLayout::Overlay) &&
              NBTV_VOICE_RADE_V2 == nbtv::duplex_voice_codec(nbtv::DuplexLayout::Standard));
static_assert(NBTV_VOICE_SIGNAL_RATE == nbtv::kDuplexVoiceRate);

int nbtv_voice_available(void) { return 1; }

const char* nbtv_voice_description(void) { return "FreeDV RADE V1 + V2 (rade_c " NBTV_VOICE_RADE_VERSION ")"; }

const char* nbtv_voice_codec_name(int codec) {
    return codec == NBTV_VOICE_RADE_V1 ? "FreeDV RADE V1" : codec == NBTV_VOICE_RADE_V2 ? "FreeDV RADE V2" : "";
}

int nbtv_voice_gap_samples(int codec) { return nbtv_rade_gap_samples(codec); }

/* ---- 發射 ---- */

nbtv_voice_tx* nbtv_voice_tx_create(int codec) {
    nbtv_voice_tx* tx = new nbtv_voice_tx;
    tx->codec = nbtv_rade_tx_open(codec);
    if (tx->codec == nullptr) {
        delete tx;
        return nullptr;
    }
    tx->block.resize(2 * static_cast<std::size_t>(nbtv_rade_tx_max_samples(tx->codec)));
    return tx;
}

void nbtv_voice_tx_destroy(nbtv_voice_tx* tx) {
    if (tx == nullptr) return;
    nbtv_rade_tx_close(tx->codec);
    delete tx;
}

void nbtv_voice_tx_reset(nbtv_voice_tx* tx) {
    if (tx == nullptr) return;
    nbtv_rade_tx_reset(tx->codec);
    tx->down.reset();
    tx->speech.clear();
    tx->iq.clear();
    tx->iq_read = 0;
    tx->finished = false;
}

void nbtv_voice_tx_push(nbtv_voice_tx* tx, const float* speech, int count) {
    if (tx == nullptr || speech == nullptr || count <= 0 || tx->finished) return;
    drop_consumed(tx->iq, tx->iq_read);
    nbtv::clean_samples(std::span<const float>(speech, static_cast<std::size_t>(count)), tx->clean);
    for (float& v : tx->clean) v = std::clamp(v, -1.0f, 1.0f);
    tx->down.process(tx->clean, tx->speech);
    std::size_t used = 0;
    for (; tx->speech.size() - used >= NBTV_RADE_SPEECH_FRAME; used += NBTV_RADE_SPEECH_FRAME) {
        const int n = nbtv_rade_tx_speech(tx->codec, tx->speech.data() + used, tx->block.data());
        tx->iq.insert(tx->iq.end(), tx->block.begin(), tx->block.begin() + 2 * n);
    }
    tx->speech.erase(tx->speech.begin(), tx->speech.begin() + static_cast<std::ptrdiff_t>(used));
}

void nbtv_voice_tx_finish(nbtv_voice_tx* tx) {
    if (tx == nullptr || tx->finished) return;
    tx->finished = true;
    // 不滿一個語音幀的那幾個取樣補零成一幀
    if (!tx->speech.empty()) {
        tx->speech.resize(NBTV_RADE_SPEECH_FRAME, 0.0f);
        const int n = nbtv_rade_tx_speech(tx->codec, tx->speech.data(), tx->block.data());
        tx->iq.insert(tx->iq.end(), tx->block.begin(), tx->block.begin() + 2 * n);
        tx->speech.clear();
    }
    const int n = nbtv_rade_tx_finish(tx->codec, tx->block.data());
    tx->iq.insert(tx->iq.end(), tx->block.begin(), tx->block.begin() + 2 * n);
}

int nbtv_voice_tx_available(const nbtv_voice_tx* tx) { return tx != nullptr ? static_cast<int>((tx->iq.size() - tx->iq_read) / 2) : 0; }

int nbtv_voice_tx_read(nbtv_voice_tx* tx, float* iq, int capacity) {
    if (tx == nullptr || iq == nullptr || capacity <= 0) return 0;
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(capacity), (tx->iq.size() - tx->iq_read) / 2);
    std::copy_n(tx->iq.begin() + static_cast<std::ptrdiff_t>(tx->iq_read), 2 * count, iq);
    tx->iq_read += 2 * count;
    return static_cast<int>(count);
}

/* ---- 接收 ---- */

nbtv_voice_rx* nbtv_voice_rx_create(int codec) {
    nbtv_voice_rx* rx = new nbtv_voice_rx;
    rx->codec = nbtv_rade_rx_open(codec);
    if (rx->codec == nullptr) {
        delete rx;
        return nullptr;
    }
    rx->decoded.resize(static_cast<std::size_t>(nbtv_rade_rx_max_speech(rx->codec)));
    return rx;
}

void nbtv_voice_rx_destroy(nbtv_voice_rx* rx) {
    if (rx == nullptr) return;
    nbtv_rade_rx_close(rx->codec);
    delete rx;
}

void nbtv_voice_rx_reset(nbtv_voice_rx* rx) {
    if (rx == nullptr) return;
    nbtv_rade_rx_reset(rx->codec);
    rx->up.reset();
    rx->iq.clear();
    rx->speech.clear();
    rx->speech_read = 0;
    rx->overs_ended = 0;
}

void nbtv_voice_rx_push(nbtv_voice_rx* rx, const float* iq, int count) {
    if (rx == nullptr || iq == nullptr || count <= 0) return;
    drop_consumed(rx->speech, rx->speech_read);
    const std::size_t before = rx->iq.size();
    rx->iq.resize(before + 2 * static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < 2 * static_cast<std::size_t>(count); ++i) rx->iq[before + i] = nbtv::clean_sample(iq[i]);
    std::size_t used = 0;
    for (;;) {
        const std::size_t need = static_cast<std::size_t>(nbtv_rade_rx_need(rx->codec));
        if ((rx->iq.size() - used) / 2 < need) break;
        int ended = 0;
        const int n = nbtv_rade_rx_push(rx->codec, rx->iq.data() + used, rx->decoded.data(), &ended);
        used += 2 * need;
        rx->overs_ended += ended;
        if (n > 0) rx->up.process(std::span<const float>(rx->decoded.data(), static_cast<std::size_t>(n)), rx->speech);
    }
    rx->iq.erase(rx->iq.begin(), rx->iq.begin() + static_cast<std::ptrdiff_t>(used));
}

int nbtv_voice_rx_available(const nbtv_voice_rx* rx) { return rx != nullptr ? static_cast<int>(rx->speech.size() - rx->speech_read) : 0; }

int nbtv_voice_rx_read(nbtv_voice_rx* rx, float* speech, int capacity) {
    if (rx == nullptr || speech == nullptr || capacity <= 0) return 0;
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(capacity), rx->speech.size() - rx->speech_read);
    for (std::size_t i = 0; i < count; ++i) speech[i] = std::clamp(rx->speech[rx->speech_read + i], -1.0f, 1.0f);
    rx->speech_read += count;
    return static_cast<int>(count);
}

void nbtv_voice_rx_get_status(const nbtv_voice_rx* rx, nbtv_voice_rx_status* out) {
    if (out == nullptr) return;
    *out = nbtv_voice_rx_status{};
    if (rx == nullptr) return;
    out->synced = nbtv_rade_rx_synced(rx->codec);
    out->snr_db = nbtv_rade_rx_snr_db(rx->codec);
    out->clock_ppm = nbtv_rade_rx_clock_ppm(rx->codec);
    out->overs_ended = rx->overs_ended;
}

#else  // 沒有把編解碼器編進來

int nbtv_voice_available(void) { return 0; }
const char* nbtv_voice_description(void) { return ""; }
const char* nbtv_voice_codec_name(int) { return ""; }
int nbtv_voice_gap_samples(int) { return 0; }

nbtv_voice_tx* nbtv_voice_tx_create(int) { return nullptr; }
void nbtv_voice_tx_destroy(nbtv_voice_tx*) {}
void nbtv_voice_tx_reset(nbtv_voice_tx*) {}
void nbtv_voice_tx_push(nbtv_voice_tx*, const float*, int) {}
void nbtv_voice_tx_finish(nbtv_voice_tx*) {}
int nbtv_voice_tx_available(const nbtv_voice_tx*) { return 0; }
int nbtv_voice_tx_read(nbtv_voice_tx*, float*, int) { return 0; }

nbtv_voice_rx* nbtv_voice_rx_create(int) { return nullptr; }
void nbtv_voice_rx_destroy(nbtv_voice_rx*) {}
void nbtv_voice_rx_reset(nbtv_voice_rx*) {}
void nbtv_voice_rx_push(nbtv_voice_rx*, const float*, int) {}
int nbtv_voice_rx_available(const nbtv_voice_rx*) { return 0; }
int nbtv_voice_rx_read(nbtv_voice_rx*, float*, int) { return 0; }
void nbtv_voice_rx_get_status(const nbtv_voice_rx*, nbtv_voice_rx_status* out) {
    if (out != nullptr) *out = nbtv_voice_rx_status{};
}

#endif
