/*
 * hf-nbtv 核心函式庫的 C API。
 *
 * 各平台的外殼（macOS／iOS 的 Swift、Android 的 Kotlin、桌面的 C++）只透過這個標頭使用核心。
 * 核心本身不開執行緒、不碰音訊裝置：外殼把收到的音訊取樣推進來、取回畫面；把畫面推進來、取回要播放的音訊取樣。
 *
 * 執行緒：任何函式都可以在任何執行緒呼叫，但同一個物件（nbtv_tx、nbtv_rx、nbtv_spectrum）
 * 不能同時被兩個執行緒使用；需要的話由外殼自己加鎖。不同的物件互不相干。
 *
 * 音訊：單聲道 float，滿刻度是 ±1.0。取樣率是 12000（數據機內部的取樣率）或 48000（核心做 4:1 的重取樣）。
 * 畫面：96×72，8 位元 RGB，由左上角開始逐列排列，共 NBTV_IMAGE_RGB_BYTES 個位元組。
 */
#ifndef NBTV_H
#define NBTV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NBTV_VERSION_MAJOR 0
#define NBTV_VERSION_MINOR 1
#define NBTV_VERSION_PATCH 0

/* 畫面大小（像素）。 */
#define NBTV_IMAGE_WIDTH 96
#define NBTV_IMAGE_HEIGHT 72
#define NBTV_IMAGE_RGB_BYTES (NBTV_IMAGE_WIDTH * NBTV_IMAGE_HEIGHT * 3)

/* 呼號最多 9 個字元：數字、大寫字母與 '/'、'-'、'.'。 */
#define NBTV_CALLSIGN_MAX 9

/* 數位模式一張影像最多的位元組數（模式 2、4）。 */
#define NBTV_MAX_IMAGE_BYTES 1473

/* 核心的版本字串，例如 "0.1.0"。 */
const char *nbtv_version_string(void);

/* ------------------------------------------------------------------ 模式 */

typedef enum nbtv_mode_kind {
    NBTV_MODE_DIGITAL = 0, /* OFDM + LDPC，每幀一張 AV1 影像 */
    NBTV_MODE_ANALOG = 1,  /* 相干類比：直接傳 DCT 係數 */
    NBTV_MODE_LEARNED = 2  /* 學習式（實驗中）：類神經網路把畫面變成通道上的值；收發兩端都要先設定模型 */
} nbtv_mode_kind;

/* 一個傳輸模式的幀結構。時間與取樣數以數據機取樣率 12 kHz 計。 */
typedef struct nbtv_mode_info {
    int id;                /* 空中的模式編號：數位 0–5，類比 8–10，學習式 11 */
    nbtv_mode_kind kind;
    int payload_symbols;   /* 每幀的酬載符號數 */
    int frame_symbols;     /* 含開頭的 S0、S1 */
    int frame_samples;     /* 每幀的取樣數 */
    double frame_seconds;  /* 每張畫面的時間 */
    int data_cells;        /* 酬載裡扣掉導頻後的資料格數 */

    /* 只有數位模式使用，類比為 0 */
    int bits_per_cell;     /* QPSK 2、16QAM 4、64QAM 6 */
    int code_rate_num;     /* LDPC 名目碼率的分子 */
    int code_rate_den;     /* LDPC 名目碼率的分母 */
    int ldpc_info_bits;    /* LDPC 資訊位元數 K */
    int payload_bytes;     /* CRC-32 保護的位元組數 */
    int image_bytes;       /* 一張影像最多能用的位元組數 */
    int padding_cells;     /* 碼字之外補滿幀用的格數 */

    /* 類比與學習式模式使用，數位為 0 */
    int header_cells;      /* 標頭佔用的格數 */
    int analog_values;     /* 標頭之外可以放的實數個數（每格兩個） */
    int analog_coefficients; /* 類比：實際傳送的係數個數（全部是 20736 個）；其餘為 0 */
    int learned_values;    /* 學習式：一張畫面變成幾個實數（模型的輸出要一樣多）；其餘為 0 */
} nbtv_mode_info;

/* 已定義的模式個數。 */
int nbtv_mode_count(void);

/* 依序列舉模式，index 從 0 到 nbtv_mode_count() - 1。成功回傳 0，否則 -1。 */
int nbtv_mode_at(int index, nbtv_mode_info *out);

/* 以空中的模式編號查詢。成功回傳 0；編號沒有定義時回傳 -1。 */
int nbtv_mode_by_id(int id, nbtv_mode_info *out);

/* ------------------------------------------------------------------ 學習式模式的模型 */

/*
 * 學習式模式（NBTV_MODE_LEARNED）的編解碼是一個類神經網路，權重放在獨立的檔案裡（models/ 底下的 .bin）。
 * 收發兩端必須用同一份權重。外殼把檔案內容讀進記憶體後交給 nbtv_model_load，再設定給發射機與接收機；
 * 同一個模型可以給多個物件用，設定之後就可以 nbtv_model_destroy（物件自己會留著）。
 * 模型載入後不會再改變，可以同時被不同執行緒上的物件使用。
 */
typedef struct nbtv_model nbtv_model;

/* data 是權重檔的內容，呼叫之後可以釋放。不是權重檔、檔案損壞或版本不認得時回傳 NULL。 */
nbtv_model *nbtv_model_load(const uint8_t *data, size_t size);
void nbtv_model_destroy(nbtv_model *model);

/* 權重檔的識別碼（檔案內容的 CRC-32）。收發兩端的識別碼相同才解得出畫面。 */
uint32_t nbtv_model_id(const nbtv_model *model);

/* 一張畫面變成幾個實數；要和模式的 learned_values 相同才能用在那個模式。 */
int nbtv_model_values(const nbtv_model *model);

/* ------------------------------------------------------------------ 呼號 */

/*
 * 把使用者輸入整理成可以傳送的呼號：轉大寫、丟掉不能用的字元、截到 9 個字元。
 * out 至少要有 NBTV_CALLSIGN_MAX + 1 個位元組。回傳整理後的長度。
 */
int nbtv_callsign_normalize(const char *text, char *out);

/* ------------------------------------------------------------------ 影像 */

/*
 * 數位模式的影像是 AV1 幀內編碼（4:4:4）。空中只傳 AV1 的資料（下面稱為「影像酬載」），
 * 不傳 AVIF 檔的外殼；需要存檔或分享時再用 nbtv_avif_from_payload() 還原成標準的 .avif 檔。
 */

/* 這個建置有沒有內含影像編解碼器（libavif）。沒有的話，nbtv_image_encode／decode 都會失敗。 */
int nbtv_has_image_codec(void);

/*
 * 把一張畫面壓成不超過 max_bytes 的影像酬載（對品質做二分搜尋，最多編 7 次）。
 * speed 是編碼器的速度檔 0（最慢、畫質最好）到 10，負值表示用預設值 3。
 * 回傳酬載的位元組數；放不下、out 太小或沒有編解碼器時回傳 -1。
 */
int nbtv_image_encode(const uint8_t *rgb, int max_bytes, int speed, uint8_t *out, int out_capacity);

/* 影像酬載（尾端可以有補零）解成畫面，rgb 要有 NBTV_IMAGE_RGB_BYTES 個位元組。成功回傳 0，否則 -1。 */
int nbtv_image_decode(const uint8_t *payload, int length, uint8_t *rgb);

/* 還原成 .avif 檔時多出來的位元組數：275 位元組的外殼加 2 位元組的時序分隔。 */
#define NBTV_AVIF_OVERHEAD 277

/*
 * 影像酬載 → 標準的 .avif 檔（不需要編解碼器）。out 的容量至少要 length + NBTV_AVIF_OVERHEAD。
 * 回傳檔案的位元組數；酬載不是合法的 96×72 AV1 畫面或 out 太小時回傳 -1。
 */
int nbtv_avif_from_payload(const uint8_t *payload, int length, uint8_t *out, int out_capacity);

/* ------------------------------------------------------------------ 發射 */

typedef struct nbtv_tx nbtv_tx;

/* sample_rate 是 12000 或 48000；其他值回傳 NULL。預設模式 1、沒有呼號。 */
nbtv_tx *nbtv_tx_create(int sample_rate);
void nbtv_tx_destroy(nbtv_tx *tx);

/* 換模式，下一幀生效。模式編號沒有定義時回傳 -1。 */
int nbtv_tx_set_mode(nbtv_tx *tx, int mode_id);
int nbtv_tx_mode(const nbtv_tx *tx);

/* 呼號會先經過 nbtv_callsign_normalize。 */
void nbtv_tx_set_callsign(nbtv_tx *tx, const char *callsign);

/* 影像編碼器的速度檔，見 nbtv_image_encode。 */
void nbtv_tx_set_encoder_speed(nbtv_tx *tx, int speed);

/* 學習式模式用的模型；model 是 NULL 表示拿掉。沒有設定模型時，學習式模式編不出幀。 */
void nbtv_tx_set_model(nbtv_tx *tx, const nbtv_model *model);

/*
 * 包絡峰均比的目標（dB）：削波門檻比平均包絡功率高這麼多。0 表示用各模式的預設值
 * （QPSK 5、16QAM 6、64QAM、類比與學習式 7），負值表示不削波。
 */
void nbtv_tx_set_papr_db(nbtv_tx *tx, float papr_db);

/* 目前的模式下，一幀有幾個取樣（以這個物件的取樣率計）。 */
int nbtv_tx_frame_samples(const nbtv_tx *tx);

typedef struct nbtv_tx_frame_info {
    int mode_id;
    int sequence;      /* 這一幀的幀序號 0..255 */
    int image_bytes;   /* 數位：壓縮後的位元組數；類比：0 */
    int quantizer;     /* 數位：編碼器選到的量化參數 0（最好）..63 */
    int coefficients;  /* 類比：傳送的係數個數；其餘：0 */
} nbtv_tx_frame_info;

/*
 * 把一張畫面做成一幀音訊。samples 的容量至少要 nbtv_tx_frame_samples()；回傳寫入的取樣數，失敗回傳 -1
 * （數位模式沒有編解碼器、畫面壓不進去、學習式模式沒有設定模型或模型不合、容量不足）。幀與幀可以直接相接連續播放。
 * 輸出的峰值不超過 ±1.0；音量由外殼自己調整。
 * preview_rgb 不是 NULL 時，填入「沒有雜訊時對方看到的畫面」：數位是壓縮再解開的結果，類比是只留傳送係數的結果，
 * 學習式是編碼後直接解回來的結果（要多花一次解碼的時間）。
 * info 可以是 NULL。
 */
int nbtv_tx_encode_image(nbtv_tx *tx, const uint8_t *rgb, float *samples, int capacity, uint8_t *preview_rgb,
                         nbtv_tx_frame_info *info);

/*
 * 數位模式：直接給影像酬載（外殼自己做影像編碼時使用）。length 不能超過模式的 image_bytes。
 * 其餘同 nbtv_tx_encode_image。
 */
int nbtv_tx_encode_payload(nbtv_tx *tx, const uint8_t *payload, int length, float *samples, int capacity,
                           nbtv_tx_frame_info *info);

/*
 * 取消最近一次 nbtv_tx_encode_image／nbtv_tx_encode_payload 編出來的那一幀：發射機回到編那一幀之前的狀態
 * （幀序號與重取樣濾波器），就像沒有編過一樣。那一幀的取樣不可以再送出去。
 * 每編一幀只能取消一次，而且要在下一次編碼或 nbtv_tx_finish 之前。成功回傳 0，沒有可以取消的幀回傳 -1。
 *
 * 用途：為了讓幀與幀之間沒有空隙，外殼會提早把下一幀編好；如果使用者在它開始播放之前按了停止，
 * 先取消它，nbtv_tx_finish 給的才是最後真正送出去那一幀的尾巴。
 */
int nbtv_tx_discard_last(nbtv_tx *tx);

/*
 * 發射結束時呼叫：取出重取樣濾波器裡還沒送完的尾巴（只有 48 kHz 時才有，約 24 個取樣）。
 * 回傳寫入的取樣數。之後可以再開始新的發射。
 */
int nbtv_tx_finish(nbtv_tx *tx, float *samples, int capacity);

/* ------------------------------------------------------------------ 接收 */

typedef struct nbtv_rx nbtv_rx;

/* sample_rate 是 12000 或 48000；其他值回傳 NULL。 */
nbtv_rx *nbtv_rx_create(int sample_rate);
void nbtv_rx_destroy(nbtv_rx *rx);

/* 清掉所有狀態，回到搜尋同步訊號（例如發射結束、重新開始接收時）。設定過的模型會留著。 */
void nbtv_rx_reset(nbtv_rx *rx);

/*
 * 學習式模式用的模型；model 是 NULL 表示拿掉。沒有設定模型、或對方用的是另一份權重時，
 * 學習式的幀照樣會回報（呼號、訊雜比都有），但沒有畫面：decoded = 0、model_missing = 1。
 */
void nbtv_rx_set_model(nbtv_rx *rx, const nbtv_model *model);

/* 推入收到的音訊。同步、解調與解碼都在這個呼叫裡完成，一幀結束時可能花幾十毫秒。 */
void nbtv_rx_push(nbtv_rx *rx, const float *samples, int count);

typedef struct nbtv_rx_status {
    int synced;               /* 1：已同步、正在接收一幀；0：搜尋同步訊號中 */
    int mode_id;              /* 自動偵測到的模式；未同步時是 -1 */
    int snr_valid;            /* snr_db 是否有效 */
    float snr_db;             /* 訊雜比：3 kHz 雜訊頻寬、訊號功率含導頻 */
    float freq_offset_hz;     /* 頻率偏移 */
    int frame_symbols_done;   /* 目前這一幀已經收到的符號數 */
    int frame_symbols_total;  /* 這一幀總共的符號數 */
    float input_level_db;     /* 輸入音量（均方根，dBFS） */
    int frames_decoded;       /* 累計解出來的幀數 */
    int frames_failed;        /* 累計解碼失敗的幀數 */
} nbtv_rx_status;

void nbtv_rx_get_status(const nbtv_rx *rx, nbtv_rx_status *out);

typedef struct nbtv_rx_frame {
    int mode_id;
    nbtv_mode_kind kind;
    int decoded;         /* 數位：CRC 正確；類比、學習式：有畫面。0 表示這一幀解碼失敗 */
    int header_ok;       /* 呼號與幀序號可信（類比與學習式的標頭可能壞掉而畫面仍在） */
    char callsign[NBTV_CALLSIGN_MAX + 1];
    int sequence;        /* 幀序號 0..255；header_ok 為 0 時是 -1 */
    float snr_db;
    float freq_offset_hz;
    int has_image;       /* rgb 是否有效。數位模式需要內含編解碼器，否則請自行解 image_data */
    uint8_t rgb[NBTV_IMAGE_RGB_BYTES];
    int image_bytes;     /* 數位：影像酬載的位元組數（已去掉補零）；其餘：0 */
    uint8_t image_data[NBTV_MAX_IMAGE_BYTES];
    int model_missing;   /* 學習式：收到了一幀，但沒有設定模型或權重和對方不同，所以沒有畫面 */
} nbtv_rx_frame;

/*
 * 取出一個已經處理完的幀。有的話填入 out 並回傳 1，沒有回傳 0。
 * 解出來的幀（decoded = 1）一定會回報。沒解出來的幀（decoded = 0）只有在確定對方送了一整幀時才回報：
 * 前導符號確認過、而且從頭到尾都有導頻；對方送到一半就停的幀、雜訊造成的誤判都不會回報。
 */
int nbtv_rx_poll_frame(nbtv_rx *rx, nbtv_rx_frame *out);

/* ------------------------------------------------------------------ 頻譜 */

/*
 * 瀑布圖用的頻譜：0–3000 Hz，共 NBTV_SPECTRUM_BINS 點（每點 11.72 Hz），大約每 43 ms 一列。
 * 收發兩邊都可以用：接收時推入收到的音訊，發射時推入送出的音訊。
 */
#define NBTV_SPECTRUM_BINS 257
#define NBTV_SPECTRUM_MAX_HZ 3000.0

typedef struct nbtv_spectrum nbtv_spectrum;

nbtv_spectrum *nbtv_spectrum_create(int sample_rate);
void nbtv_spectrum_destroy(nbtv_spectrum *sp);
void nbtv_spectrum_reset(nbtv_spectrum *sp);
void nbtv_spectrum_push(nbtv_spectrum *sp, const float *samples, int count);

/*
 * 取出最舊的一列，db 要有 NBTV_SPECTRUM_BINS 個 float，單位 dBFS（滿刻度的正弦波是 0 dB）。
 * 有的話回傳 1，沒有回傳 0。來不及取的列會被丟掉，只保留最近的幾十列。
 */
int nbtv_spectrum_poll(nbtv_spectrum *sp, float *db);

#ifdef __cplusplus
}
#endif

#endif /* NBTV_H */
