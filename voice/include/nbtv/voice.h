/*
 * 即時語音的編解碼器：FreeDV 的 RADE（Radio Autoencoder），V1 與 V2 兩版都有。
 *
 * 給「語音與畫面一起送」用（core/include/nbtv/nbtv.h 的 nbtv_duplex_*）：
 *   發射  麥克風的語音（48 kHz）→ nbtv_voice_tx → 語音的訊號（8 kHz 的複數、中心 1500 Hz）→ nbtv_duplex_tx_push_voice
 *   接收  nbtv_duplex_rx_read_voice → nbtv_voice_rx → 語音（48 kHz）→ 喇叭
 * 哪一種版面用哪一版：分頻的兩種（FM）用 V2，疊加的（短波的 SSB）用 V1；問核心的 nbtv_duplex_voice_codec()。
 *
 * 這一層是選用的：建置時沒有打開（-DNBTV_VOICE=ON，會另外抓 rade_c 與 Opus 的原始碼）的話，
 * nbtv_voice_available() 回傳 0，建立物件的函式都回傳 NULL，其餘的函式什麼都不做。
 * 編解碼器本身不是這個專案的：rade_c（BSD 兩條款）與 Opus（BSD 三條款），授權條文在 voice/LICENSES/。
 * RADE V1 是 FreeDV 正式發布的波形：這裡送出來的訊號和 rade_c 自己的 V1 工具互相解得開（沒有和 FreeDV 的程式本身對過）。
 * RADE V2 在 2026 年底還是測試版：它的波形與權重之後可能再改，改了之後新舊版本互相聽不到。
 *
 * 每一個物件同一時間只能給一條執行緒用；不同的物件可以各自在不同的執行緒上。
 * 語音都是單聲道的 float，滿刻度 ±1。
 */
#ifndef NBTV_VOICE_H
#define NBTV_VOICE_H

#ifdef __cplusplus
extern "C" {
#endif

#define NBTV_VOICE_SPEECH_RATE 48000 /* 語音進出的取樣率 */
#define NBTV_VOICE_SIGNAL_RATE 8000  /* 語音訊號的取樣率：複數，I、Q 交錯 */

/* 哪一版的編解碼器。數字和核心的 nbtv_duplex_voice_codec() 回傳的相同。 */
#define NBTV_VOICE_RADE_V1 1 /* 正式發布的那一版：30 個載波、約 730–2225 Hz，一段 120 ms */
#define NBTV_VOICE_RADE_V2 2 /* 測試版：約 860 Hz 寬，一段 40 ms */

/* 這份程式有沒有把編解碼器編進來：有的話回傳 1（兩版一起，沒有只編一版的）。 */
int nbtv_voice_available(void);
/* 編解碼器的名稱與版本（給「關於」之類的地方顯示），例如 "FreeDV RADE V1 + V2 (rade_c 23c3746)"；沒有編進來時是空字串。 */
const char *nbtv_voice_description(void);
/* 其中一版的名稱，例如 "FreeDV RADE V1"；codec 不對、或沒有編進來時是空字串。 */
const char *nbtv_voice_codec_name(int codec);
/*
 * 語音的訊號中途停下來再接回去時，停的長度要是幾個複數的整數倍（在 NBTV_VOICE_SIGNAL_RATE 下）：
 * V2 是一個符號（160 個，20 ms），V1 是一個數據幀（960 個，120 ms）。codec 不對、或沒有編進來時是 0。
 *
 * 用在「訊號送到一半得停一下」的時候（電腦一時忙不過來，語音還沒編出來）：停的長度是它的整數倍的話，
 * 對方的接收端不必重新同步，接著收；不是整數倍的話對方要重新同步（V2 約半秒、V1 約兩秒聽不到），剛好停在結尾前，
 * 「結束」的標記對方也收不到。訊號合成到 48 kHz 之後（nbtv_duplex_tx）也一樣，那時候是它的 6 倍個取樣。
 */
int nbtv_voice_gap_samples(int codec);

/* ------------------------------------------------------------------ 發射：語音 → 訊號 */

typedef struct nbtv_voice_tx nbtv_voice_tx;

/* codec 是 NBTV_VOICE_RADE_V1 或 NBTV_VOICE_RADE_V2；不對、或沒有編進來時回傳 NULL。 */
nbtv_voice_tx *nbtv_voice_tx_create(int codec);
void nbtv_voice_tx_destroy(nbtv_voice_tx *tx);
/* 回到剛建立的狀態（下一次發射開始之前）：還沒取走的訊號也丟掉。 */
void nbtv_voice_tx_reset(nbtv_voice_tx *tx);
/* 推入語音（48 kHz）。每湊滿一段（V2 是 40 ms，V1 是 120 ms）就多出一段訊號可以取。 */
void nbtv_voice_tx_push(nbtv_voice_tx *tx, const float *speech, int count);
/*
 * 這一次發射的語音結束了：把還沒湊滿的那一小段補上靜音送出，後面接上「結束」的標記（對方的接收端看到它就知道這一段講完了，
 * 不必等到訊號消失）。之後要再用，先 nbtv_voice_tx_reset。
 */
void nbtv_voice_tx_finish(nbtv_voice_tx *tx);
/* 可以取的訊號有幾個複數。 */
int nbtv_voice_tx_available(const nbtv_voice_tx *tx);
/* 取出訊號：最多 capacity 個複數（iq 要有 2 × capacity 個 float）；回傳取了幾個複數。均方根 V2 約 0.7、V1 約 0.91。 */
int nbtv_voice_tx_read(nbtv_voice_tx *tx, float *iq, int capacity);

/* ------------------------------------------------------------------ 接收：訊號 → 語音 */

typedef struct nbtv_voice_rx nbtv_voice_rx;

nbtv_voice_rx *nbtv_voice_rx_create(int codec);
void nbtv_voice_rx_destroy(nbtv_voice_rx *rx);
/* 回到剛建立的狀態（重新開始接收時）：還沒取走的語音也丟掉。 */
void nbtv_voice_rx_reset(nbtv_voice_rx *rx);
/*
 * 推入收到的訊號：count 個複數（iq 有 2 × count 個 float），8 kHz、中心 1500 Hz。大小不拘（裡面會自己調整）。
 * V1 的接收端自己找得到大約 ±50 Hz 以內的頻偏（SSB 兩邊的頻率差一點也收得到；再多它就同步不了）。
 */
void nbtv_voice_rx_push(nbtv_voice_rx *rx, const float *iq, int count);
/* 可以取的語音有幾個取樣（48 kHz）。沒有同步到訊號的時候不會有語音出來。 */
int nbtv_voice_rx_available(const nbtv_voice_rx *rx);
/* 取出語音：最多 capacity 個取樣；回傳取了幾個。 */
int nbtv_voice_rx_read(nbtv_voice_rx *rx, float *speech, int capacity);

typedef struct nbtv_voice_rx_status {
    int synced;      /* 目前是不是同步到語音的訊號 */
    float snr_db;    /* 訊雜比的估計（3 kHz 的雜訊頻寬），synced 時有效 */
    float clock_ppm; /* 兩邊取樣時脈的差（ppm），同步之後慢慢收斂；V1 不估這個數字，一律是 0 */
    int overs_ended; /* 從建立（或上一次 nbtv_voice_rx_reset）到現在，收到幾次「結束」的標記 */
} nbtv_voice_rx_status;

void nbtv_voice_rx_get_status(const nbtv_voice_rx *rx, nbtv_voice_rx_status *out);

#ifdef __cplusplus
}
#endif

#endif /* NBTV_VOICE_H */
