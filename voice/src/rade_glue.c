/*
 * rade_glue.h 的實作。流程照 rade_c 自己的兩個範例（rade_tx_wav.c、rade_rx_wav.c）：
 *   發射  語音幀 → LPCNet 的特徵（每幀 36 個數）→ 湊滿 rade_n_features_in_out() 個 → rade_tx → 訊號
 *   接收  訊號（每次 rade_nin() 個）→ rade_rx → 特徵 → FARGAN 合成語音（開頭 5 幀拿來暖機，不出聲）
 * 和它們一樣，這個檔案不含 Opus 的 config.h：語音的特徵與合成走的是不分 CPU 的那條路。
 */
#include "rade_glue.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rade_api.h"
#include "rade_v2_ofdm.h" /* 符號的長度（rade_c 對外的 rade_api.h 沒有給） */

#include "arch.h"
#include "cpu_support.h"
#include "fargan.h"
#include "lpcnet.h"

#define FEATURE_SIZE NB_TOTAL_FEATURES /* 一個語音幀的特徵：36 個數（合成只用前 20 個） */
#define WARMUP_FRAMES 5                /* FARGAN 開始合成之前要先看幾幀 */

#if LPCNET_FRAME_SIZE != NBTV_RADE_SPEECH_FRAME
#error "語音幀的長度和 LPCNet 的不一樣"
#endif

int nbtv_rade_symbol_samples(void) { return RADE_V2_SYM_LEN; }

static struct rade *open_context(void) {
    static char name[] = "";
    rade_initialize();
    return rade_open(name, RADE_MODE_V2 | RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
}

/* ------------------------------------------------------------------ 發射 */

struct nbtv_rade_tx {
    struct rade *rade;
    LPCNetEncState *encoder;
    int arch;
    int frames_per_step; /* 幾個語音幀湊成一段 */
    int step_samples;    /* 一段訊號有幾個複數 */
    int end_samples;     /* 「結束」的標記有幾個複數 */
    int frames;          /* 已經湊了幾個語音幀 */
    float *features;
    RADE_COMP *signal;
};

static int copy_signal(const RADE_COMP *signal, int count, float *iq) {
    for (int i = 0; i < count; i++) {
        iq[2 * i] = signal[i].real;
        iq[2 * i + 1] = signal[i].imag;
    }
    return count;
}

nbtv_rade_tx *nbtv_rade_tx_open(void) {
    nbtv_rade_tx *tx = (nbtv_rade_tx *)calloc(1, sizeof *tx);
    if (tx == NULL) return NULL;
    tx->rade = open_context();
    tx->encoder = lpcnet_encoder_create();
    if (tx->rade != NULL) {
        tx->frames_per_step = rade_n_features_in_out(tx->rade) / FEATURE_SIZE;
        tx->step_samples = rade_n_tx_out(tx->rade);
        tx->end_samples = rade_n_tx_eoo_out(tx->rade);
        tx->features = (float *)calloc((size_t)rade_n_features_in_out(tx->rade), sizeof(float));
        const int most = tx->step_samples > tx->end_samples ? tx->step_samples : tx->end_samples;
        tx->signal = (RADE_COMP *)calloc((size_t)most, sizeof(RADE_COMP));
    }
    if (tx->rade == NULL || tx->encoder == NULL || tx->features == NULL || tx->signal == NULL || tx->frames_per_step <= 0) {
        nbtv_rade_tx_close(tx);
        return NULL;
    }
    tx->arch = opus_select_arch();
    return tx;
}

void nbtv_rade_tx_close(nbtv_rade_tx *tx) {
    if (tx == NULL) return;
    if (tx->encoder != NULL) lpcnet_encoder_destroy(tx->encoder);
    if (tx->rade != NULL) rade_close(tx->rade);
    free(tx->features);
    free(tx->signal);
    free(tx);
}

void nbtv_rade_tx_reset(nbtv_rade_tx *tx) {
    if (tx == NULL) return;
    /* rade_c 沒有「重來」的函式：把發射那一半重新初始化（rade_open 做的就是這件事） */
    rade_tx_v2_init(&tx->rade->tx_v2, (tx->rade->flags & RADE_NO_TX_BPF) ? 0 : 1);
    lpcnet_encoder_init(tx->encoder);
    tx->frames = 0;
}

int nbtv_rade_tx_max_samples(const nbtv_rade_tx *tx) { return tx != NULL ? tx->step_samples + tx->end_samples : 0; }

int nbtv_rade_tx_speech(nbtv_rade_tx *tx, const float *speech, float *iq) {
    opus_int16 pcm[LPCNET_FRAME_SIZE];
    for (int i = 0; i < LPCNET_FRAME_SIZE; i++) {
        float v = speech[i] * 32768.0f;
        if (!(v > -32767.0f)) v = -32767.0f; /* 非數值也落到這裡 */
        if (v > 32767.0f) v = 32767.0f;
        pcm[i] = (opus_int16)floor(0.5 + (double)v);
    }
    lpcnet_compute_single_frame_features(tx->encoder, pcm, &tx->features[tx->frames * FEATURE_SIZE], tx->arch);
    if (++tx->frames < tx->frames_per_step) return 0;
    tx->frames = 0;
    return copy_signal(tx->signal, rade_tx(tx->rade, tx->signal, tx->features), iq);
}

int nbtv_rade_tx_finish(nbtv_rade_tx *tx, float *iq) {
    int count = 0;
    if (tx->frames > 0) {
        memset(&tx->features[tx->frames * FEATURE_SIZE], 0, (size_t)(tx->frames_per_step - tx->frames) * FEATURE_SIZE * sizeof(float));
        tx->frames = 0;
        count += copy_signal(tx->signal, rade_tx(tx->rade, tx->signal, tx->features), iq);
    }
    count += copy_signal(tx->signal, rade_tx_eoo(tx->rade, tx->signal), iq + 2 * count);
    return count;
}

/* ------------------------------------------------------------------ 接收 */

struct nbtv_rade_rx {
    struct rade *rade;
    FARGANState synth;
    int max_need;
    int max_frames; /* 一次最多解出幾個語音幀 */
    int warm;       /* 暖機已經收了幾幀；滿 WARMUP_FRAMES 之後才合成 */
    float warmup[WARMUP_FRAMES * NB_FEATURES];
    float *features;
    RADE_COMP *signal;
};

nbtv_rade_rx *nbtv_rade_rx_open(void) {
    nbtv_rade_rx *rx = (nbtv_rade_rx *)calloc(1, sizeof *rx);
    if (rx == NULL) return NULL;
    rx->rade = open_context();
    if (rx->rade != NULL) {
        rx->max_need = rade_nin_max(rx->rade);
        rx->max_frames = rade_n_features_in_out(rx->rade) / FEATURE_SIZE;
        rx->features = (float *)calloc((size_t)rade_n_features_in_out(rx->rade), sizeof(float));
        rx->signal = (RADE_COMP *)calloc((size_t)rx->max_need, sizeof(RADE_COMP));
    }
    if (rx->rade == NULL || rx->features == NULL || rx->signal == NULL || rx->max_frames <= 0) {
        nbtv_rade_rx_close(rx);
        return NULL;
    }
    fargan_init(&rx->synth);
    return rx;
}

void nbtv_rade_rx_close(nbtv_rade_rx *rx) {
    if (rx == NULL) return;
    if (rx->rade != NULL) rade_close(rx->rade);
    free(rx->features);
    free(rx->signal);
    free(rx);
}

void nbtv_rade_rx_reset(nbtv_rade_rx *rx) {
    if (rx == NULL) return;
    rade_rx_v2_init(&rx->rade->rx_v2, 1);
    rx->rade->rx_v2.verbose = 0;
    fargan_init(&rx->synth);
    rx->warm = 0;
}

int nbtv_rade_rx_need(nbtv_rade_rx *rx) { return rade_nin(rx->rade); }

int nbtv_rade_rx_max_need(const nbtv_rade_rx *rx) { return rx != NULL ? rx->max_need : 0; }

int nbtv_rade_rx_max_speech(const nbtv_rade_rx *rx) { return rx != NULL ? rx->max_frames * LPCNET_FRAME_SIZE : 0; }

int nbtv_rade_rx_push(nbtv_rade_rx *rx, const float *iq, float *speech, int *ended) {
    const int need = rade_nin(rx->rade);
    for (int i = 0; i < need; i++) {
        rx->signal[i].real = iq[2 * i];
        rx->signal[i].imag = iq[2 * i + 1];
    }
    int has_end = 0;
    const int values = rade_rx(rx->rade, rx->features, &has_end, NULL, rx->signal);
    if (ended != NULL && has_end) *ended = 1;
    int written = 0;
    for (int frame = 0; frame < values / FEATURE_SIZE; frame++) {
        const float *features = &rx->features[frame * FEATURE_SIZE];
        if (rx->warm < WARMUP_FRAMES) {
            memcpy(&rx->warmup[rx->warm * NB_FEATURES], features, NB_FEATURES * sizeof(float));
            if (++rx->warm == WARMUP_FRAMES) {
                float zeros[FARGAN_CONT_SAMPLES];
                memset(zeros, 0, sizeof zeros);
                fargan_cont(&rx->synth, zeros, rx->warmup);
            }
            continue;
        }
        fargan_synthesize(&rx->synth, speech + written, features);
        written += LPCNET_FRAME_SIZE;
    }
    return written;
}

int nbtv_rade_rx_synced(nbtv_rade_rx *rx) { return rade_sync(rx->rade) != 0; }

float nbtv_rade_rx_snr_db(nbtv_rade_rx *rx) { return rade_snrdB_3k_est(rx->rade); }

float nbtv_rade_rx_clock_ppm(nbtv_rade_rx *rx) { return rade_clock_offset_ppm(rx->rade); }
