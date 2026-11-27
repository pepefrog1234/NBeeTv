/*
 * 接 rade_c 與 Opus（LPCNet 的特徵、FARGAN 的合成）的那一層，用 C 寫：它們的標頭不是給 C++ 用的。
 * voice.cpp 只透過這裡的函式用到它們。語音都是 16 kHz、一個語音幀 160 個取樣（10 ms）、滿刻度 ±1 的 float；
 * 訊號是 8 kHz 的複數，I、Q 交錯。
 */
#ifndef NBTV_RADE_GLUE_H
#define NBTV_RADE_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

#define NBTV_RADE_SPEECH_FRAME 160

/* 哪一版的 RADE（rade_c 兩版都有，權重也都編在裡面）。數字和 nbtv/voice.h 的 NBTV_VOICE_RADE_V1、_V2 相同。 */
#define NBTV_RADE_V1 1
#define NBTV_RADE_V2 2

/*
 * 訊號中途停下來再接回去時，停的長度要是幾個複數的整數倍，對方的接收端才不必重新同步。
 * V2 是一個符號（含循環字首，160 個）；V1 是一個數據幀（4 個資料符號加 1 個導頻符號，960 個）。version 不對回傳 0。
 */
int nbtv_rade_gap_samples(int version);

/* ---- 發射 ---- */
typedef struct nbtv_rade_tx nbtv_rade_tx;

/* version 是 NBTV_RADE_V1 或 NBTV_RADE_V2；不對回傳 NULL。 */
nbtv_rade_tx *nbtv_rade_tx_open(int version);
void nbtv_rade_tx_close(nbtv_rade_tx *tx);
void nbtv_rade_tx_reset(nbtv_rade_tx *tx);
/* 一次呼叫最多寫出幾個複數（iq 要有兩倍這麼多個 float）。 */
int nbtv_rade_tx_max_samples(const nbtv_rade_tx *tx);
/* 一個語音幀。湊滿一段時把訊號寫到 iq，回傳複數的個數；還沒湊滿回傳 0。 */
int nbtv_rade_tx_speech(nbtv_rade_tx *tx, const float *speech, float *iq);
/* 結束：還沒湊滿的那一段補零送出，後面接上「結束」的標記。回傳複數的個數。 */
int nbtv_rade_tx_finish(nbtv_rade_tx *tx, float *iq);

/* ---- 接收 ---- */
typedef struct nbtv_rade_rx nbtv_rade_rx;

nbtv_rade_rx *nbtv_rade_rx_open(int version);
void nbtv_rade_rx_close(nbtv_rade_rx *rx);
void nbtv_rade_rx_reset(nbtv_rade_rx *rx);
/* 下一次 nbtv_rade_rx_push 要給幾個複數（每次不一定一樣：接收端靠它跟著對方的時脈），以及最多會要幾個。 */
int nbtv_rade_rx_need(nbtv_rade_rx *rx);
int nbtv_rade_rx_max_need(const nbtv_rade_rx *rx);
/* 一次呼叫最多寫出幾個語音取樣。 */
int nbtv_rade_rx_max_speech(const nbtv_rade_rx *rx);
/* 給剛好 nbtv_rade_rx_need() 個複數。回傳寫到 speech 的語音取樣數（沒有同步時是 0）；收到「結束」的標記時 *ended 設成 1。 */
int nbtv_rade_rx_push(nbtv_rade_rx *rx, const float *iq, float *speech, int *ended);
int nbtv_rade_rx_synced(nbtv_rade_rx *rx);
float nbtv_rade_rx_snr_db(nbtv_rade_rx *rx);
/* 兩邊取樣時脈的差。V1 不估這個數字：一律是 0。 */
float nbtv_rade_rx_clock_ppm(nbtv_rade_rx *rx);

#ifdef __cplusplus
}
#endif

#endif /* NBTV_RADE_GLUE_H */
