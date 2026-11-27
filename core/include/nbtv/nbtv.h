/*
 * hf-nbtv 核心函式庫的 C API。
 *
 * 各平台的外殼（macOS／iOS 的 Swift、Android 的 Kotlin、桌面的 C++）只透過這個標頭使用核心。
 * 核心本身不碰音訊裝置：外殼把收到的音訊取樣推進來、取回畫面；把畫面推進來、取回要播放的音訊取樣。
 * 工作都在呼叫的那條執行緒上做完才返回；只有畫面的編碼與解碼會另外用幾條執行緒一起算（見下面「運算」一節）。
 *
 * 畫面的編解碼是類神經網路（「學習式」的編碼）：每個模式各有一份權重檔，收發兩端都要先載入、設定給
 * 發射機與接收機，見下面「模型」一節。
 *
 * 執行緒：任何函式都可以在任何執行緒呼叫，但同一個物件（nbtv_tx、nbtv_rx、nbtv_spectrum）
 * 不能同時被兩個執行緒使用；需要的話由外殼自己加鎖。不同的物件互不相干。
 *
 * 音訊：單聲道 float，滿刻度是 ±1.0。取樣率是 12000（數據機內部的取樣率）或 48000（核心做 4:1 的重取樣）。
 * 推進來的取樣（nbtv_rx_push、nbtv_spectrum_push）裡如果有非有限值（NaN、無限大），會被當成 0；
 * 絕對值超過 100 的會被限制在 ±100。交出來的讀數（音量、訊雜比、頻率偏移、頻譜）因此一定是有限值。
 * 畫面：8 位元 RGB，由左上角開始逐列排列，共「寬 × 高 × 3」個位元組。大小由模式決定（nbtv_mode_info 的
 * image_width、image_height）：模式 3 與 11–14 是 96×72，模式 4–7 與 15（FM 用的大畫面）是 192×144。
 */
#ifndef NBTV_H
#define NBTV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NBTV_VERSION_MAJOR 0
#define NBTV_VERSION_MINOR 2
#define NBTV_VERSION_PATCH 1

/* 標準的畫面大小（像素）：模式 11–14 的畫面。 */
#define NBTV_IMAGE_WIDTH 96
#define NBTV_IMAGE_HEIGHT 72
#define NBTV_IMAGE_RGB_BYTES (NBTV_IMAGE_WIDTH * NBTV_IMAGE_HEIGHT * 3)

/* 最大的畫面：任何模式的畫面都不超過這個大小（模式 4–7 與 15 就是這麼大）。準備緩衝區時用。 */
#define NBTV_IMAGE_MAX_WIDTH 192
#define NBTV_IMAGE_MAX_HEIGHT 144
#define NBTV_IMAGE_MAX_RGB_BYTES (NBTV_IMAGE_MAX_WIDTH * NBTV_IMAGE_MAX_HEIGHT * 3)

/* 呼號最多 9 個字元：數字、大寫字母與 '/'、'-'、'.'。 */
#define NBTV_CALLSIGN_MAX 9

/* 核心的版本字串，例如 "0.1.0"。 */
const char *nbtv_version_string(void);

/* ------------------------------------------------------------------ 模式 */

/*
 * 一個傳輸模式的幀結構。時間與取樣數以數據機取樣率 12 kHz 計。
 * 每一幀是一張畫面：類神經網路把畫面變成 learned_values 個實數，兩個一組放進資料格。
 */
typedef struct nbtv_mode_info {
    int id;                /* 空中的模式編號（0..15 之間）：3–7 與 11–15。其餘的沒有定義，草案裡用過它們的數位與類比模式已經移除 */
    int payload_symbols;   /* 每幀的酬載符號數 */
    int frame_symbols;     /* 含開頭的 S0、S1；沒有散佈導頻的幀再加結尾的一個參考符號 */
    int frame_samples;     /* 每幀的取樣數 */
    double frame_seconds;  /* 每張畫面的時間 */
    int data_cells;        /* 酬載裡的資料格數（有散佈導頻的幀要扣掉導頻） */
    int header_cells;      /* 標頭（呼號、幀序號）佔用的格數 */
    int value_capacity;    /* 標頭之外可以放的實數個數（每格兩個） */
    int learned_values;    /* 一張畫面變成幾個實數（模型的輸出要一樣多） */
    int learned_width;     /* 模型第一層的通道數（實數個數相同、大小不同的模型各用一個模式） */
    int image_width;       /* 畫面的寬與高（像素）：模式 3 與 11–14 是 96×72，模式 4–7 與 15 是 192×144 */
    int image_height;
    /* 酬載裡有沒有散佈導頻（1 有、0 沒有）。沒有的是給 FM 語音通道用的幀（模式 3–7）：同樣的時間多 2 成的資料格，
     * 但通道在一幀之內不能變、也不能有頻偏，所以不適合短波。 */
    int scattered_pilots;
} nbtv_mode_info;

/* 已定義的模式個數。 */
int nbtv_mode_count(void);

/* 依序列舉模式，index 從 0 到 nbtv_mode_count() - 1。成功回傳 0，否則 -1。 */
int nbtv_mode_at(int index, nbtv_mode_info *out);

/* 以空中的模式編號查詢。成功回傳 0；編號沒有定義時回傳 -1。 */
int nbtv_mode_by_id(int id, nbtv_mode_info *out);

/* ------------------------------------------------------------------ 模型 */

/*
 * 畫面的編解碼是一個類神經網路，權重放在獨立的檔案裡（models/ 底下的 .bin）。
 * 每個模式各有自己的模型：模型的實數個數（nbtv_model_values）、寬度（nbtv_model_width）與畫面大小
 * （nbtv_model_image_width、nbtv_model_image_height）和模式的 learned_values、learned_width、image_width、
 * image_height 都相同，就是給它用的。
 * 收發兩端必須用同一份權重。外殼把檔案內容讀進記憶體後交給 nbtv_model_load，再設定給發射機與接收機
 * （要用幾個模式就設定幾個）；同一個模型可以給多個物件用，設定之後就可以 nbtv_model_destroy（物件自己會留著）。
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

/* 模型第一層的通道數（愈大的模型愈寬，運算量也愈大）；要和模式的 learned_width 相同才能用在那個模式。 */
int nbtv_model_width(const nbtv_model *model);

/* 模型的畫面大小（像素）：模型是照這個大小訓練的，記在權重檔裡；要和模式的 image_width、image_height 相同才能用在那個模式。 */
int nbtv_model_image_width(const nbtv_model *model);
int nbtv_model_image_height(const nbtv_model *model);

/*
 * 把一張畫面的通道值（nbtv_tx_last_values 取出來的，count 要等於 nbtv_model_values）解回畫面：
 * 沒有雜訊時對方看到的樣子，和 nbtv_tx_encode_image 的 preview_rgb 是同一個結果。
 * rgb 要有這個模型的畫面大小 × 3 個位元組。成功回傳 0；個數不對或指標是 NULL 回傳 -1。
 *
 * 分開來是為了讓外殼把這一步放到另一條執行緒上：加寬的大畫面模型（模式 4）只用一條執行緒時編一張約 0.4 秒、解一張約 0.7 秒
 * （M4 Max）。核心少、比較慢的電腦上兩樣接在一起做的話，準備一張畫面的時間會比一幀（1.01 秒）還長，幀與幀之間就有空檔。
 * 這個函式不會改動模型，可以和別的執行緒同時使用同一個模型。
 */
int nbtv_model_decode(const nbtv_model *model, const float *values, int count, uint8_t *rgb);

/* ------------------------------------------------------------------ 合併最近幾幀 */

/*
 * 接收端可以在解每一幀之前，把前面幾幀收到的值合進來：畫面沒變的地方雜訊被平均掉，畫面變了的地方只用這一幀。
 * 訊號弱、畫面平靜時畫面比較清楚也比較穩定；訊號很好或畫面一直在變時和不合併幾乎一樣。
 * 發射端與空中格式都不變，對方不需要知道。
 * 判斷畫面變了多少的是一個小網路，權重放在獨立的檔案裡（models/ 底下），每個畫面模型各配一份。
 * 用法和模型一樣：把檔案內容交給 nbtv_merge_load，設定給接收機（nbtv_rx_set_merge），之後就可以 nbtv_merge_destroy。
 * 另外要用 nbtv_rx_set_merging 打開（預設是關的）。
 */
typedef struct nbtv_merge nbtv_merge;

/* data 是權重檔的內容，呼叫之後可以釋放。不是這種權重檔、檔案損壞或版本不認得時回傳 NULL。 */
nbtv_merge *nbtv_merge_load(const uint8_t *data, size_t size);
void nbtv_merge_destroy(nbtv_merge *merge);

/* 它配的是哪一個畫面模型（nbtv_model_id）。 */
uint32_t nbtv_merge_model_id(const nbtv_merge *merge);

/* 一共看幾幀：現在這一幀，加上前面幾幀。 */
int nbtv_merge_frames(const nbtv_merge *merge);

/* ------------------------------------------------------------------ 運算 */

/*
 * 畫面的編碼與解碼（類神經網路的推論）是核心裡最花時間的部分，會分給幾條執行緒一起算：
 * 呼叫 nbtv_tx_encode_image、nbtv_rx_push 的那一條，加上核心自己開的幾條幫手。幫手是整個行程共用的一組，
 * 第一次用到時才開，之後留著等下一張畫面。算出來的結果和只用一條執行緒時逐位元相同。
 *
 * nbtv_inference_threads 回傳最多同時用幾條（含呼叫的那一條）。預設是這台電腦的邏輯處理器數，最多 8；
 * 環境變數 NBTV_THREADS 有設的話用它。nbtv_set_inference_threads 可以改：1 是完全不開執行緒，最多 64，
 * 0 回到預設；整個行程一起生效，隨時可以改。
 *
 * nbtv_inference_kernel 回傳這台電腦上最內層的迴圈用的是哪一種寫法："avx2"（x86-64 上 CPU 與作業系統都支援
 * AVX2 與 FMA 時）或 "generic"（其他情形：一般的 C++，由編譯器向量化）。啟動後第一次用到時決定，之後不變。
 * 兩種寫法的結果只差在浮點數的末位，遠小於通道的雜訊，不影響互通。環境變數 NBTV_KERNEL=generic 可以指定不用 avx2。
 */
int nbtv_inference_threads(void);
void nbtv_set_inference_threads(int threads);
const char *nbtv_inference_kernel(void);

/* ------------------------------------------------------------------ 呼號 */

/*
 * 把使用者輸入整理成可以傳送的呼號：轉大寫、丟掉不能用的字元、截到 9 個字元。
 * out 至少要有 NBTV_CALLSIGN_MAX + 1 個位元組。回傳整理後的長度。
 */
int nbtv_callsign_normalize(const char *text, char *out);

/* ------------------------------------------------------------------ 發射 */

typedef struct nbtv_tx nbtv_tx;

/* sample_rate 是 12000 或 48000；其他值回傳 NULL。預設是模式表的第一個模式（11）、沒有呼號、沒有模型。 */
nbtv_tx *nbtv_tx_create(int sample_rate);
void nbtv_tx_destroy(nbtv_tx *tx);

/* 換模式，下一幀生效。模式編號沒有定義時回傳 -1。畫面大小跟著模式變（見 nbtv_tx_encode_image）。 */
int nbtv_tx_set_mode(nbtv_tx *tx, int mode_id);
int nbtv_tx_mode(const nbtv_tx *tx);

/* 呼號會先經過 nbtv_callsign_normalize。 */
void nbtv_tx_set_callsign(nbtv_tx *tx, const char *callsign);

/*
 * 各個模式用的模型。可以設定好幾個，每個模式用實數個數、寬度與畫面大小都和它相同的那一個；三者都相同的再設定一次會取代前一個。
 * model 是 NULL 表示全部拿掉。某個模式沒有模型時，那個模式編不出幀。
 */
void nbtv_tx_set_model(nbtv_tx *tx, const nbtv_model *model);

/* 包絡峰均比的目標（dB）：削波門檻比平均包絡功率高這麼多。0 表示用預設值（7 dB），負值表示不削波。 */
void nbtv_tx_set_papr_db(nbtv_tx *tx, float papr_db);

/* 目前的模式下，一幀有幾個取樣（以這個物件的取樣率計）。 */
int nbtv_tx_frame_samples(const nbtv_tx *tx);

typedef struct nbtv_tx_frame_info {
    int mode_id;
    int sequence;      /* 這一幀的幀序號 0..255 */
} nbtv_tx_frame_info;

/*
 * 把一張畫面做成一幀音訊。rgb 是目前這個模式的一張畫面：image_width × image_height × 3 個位元組
 * （模式 3 與 11–14 是 NBTV_IMAGE_RGB_BYTES，模式 4–7 與 15 是 NBTV_IMAGE_MAX_RGB_BYTES）。
 * samples 的容量至少要 nbtv_tx_frame_samples()；回傳寫入的取樣數，失敗回傳 -1
 * （沒有設定目前這個模式用的模型、容量不足）。幀與幀可以直接相接連續播放。
 * 輸出的峰值不超過 ±1.0；音量由外殼自己調整。
 * preview_rgb 不是 NULL 時，填入「沒有雜訊時對方看到的畫面」：編碼後直接解回來的結果（大小和 rgb 相同；要多花一次解碼的時間）。
 * info 可以是 NULL。
 */
int nbtv_tx_encode_image(nbtv_tx *tx, const uint8_t *rgb, float *samples, int capacity, uint8_t *preview_rgb,
                         nbtv_tx_frame_info *info);

/*
 * 最近一次 nbtv_tx_encode_image 編的那一張畫面的通道值（那個模式的 learned_values 個實數）。
 * 交給 nbtv_model_decode 就是 preview_rgb 會給的畫面：編碼時 preview_rgb 給 NULL、之後在別的執行緒上解，
 * 發射的這條執行緒就不必等解碼。values 的容量至少要有那麼多個；回傳寫入的個數。
 * 還沒編過、最近那一次失敗了、那一幀已經用 nbtv_tx_discard_last 取消，或容量不足，回傳 -1。
 * 要和 nbtv_tx_encode_image 在同一條執行緒上呼叫（發射機本身不能同時給兩條執行緒用）。
 */
int nbtv_tx_last_values(const nbtv_tx *tx, float *values, int capacity);

/*
 * 取消最近一次 nbtv_tx_encode_image 編出來的那一幀：發射機回到編那一幀之前的狀態
 * （幀序號與重取樣濾波器），就像沒有編過一樣。那一幀的取樣不可以再送出去。
 * 每編一幀只能取消一次，而且要在下一次編碼或 nbtv_tx_finish 之前。成功回傳 0，沒有可以取消的幀回傳 -1。
 *
 * 用途：為了讓幀與幀之間沒有空隙，外殼會提早把下一幀編好；如果使用者在它開始播放之前按了停止，
 * 先取消它，nbtv_tx_finish 給的才是最後真正送出去那一幀的尾巴。
 */
int nbtv_tx_discard_last(nbtv_tx *tx);

/*
 * 發射結束時呼叫：取出重取樣濾波器裡還沒送完的尾巴（只有 48 kHz 時才有，約 24 個取樣；
 * 打開了 nbtv_tx_set_duplex 的話比較長，容量要有 nbtv_tx_tail_samples() 那麼多）。
 * 回傳寫入的取樣數。之後可以再開始新的發射。
 */
int nbtv_tx_finish(nbtv_tx *tx, float *samples, int capacity);

/* nbtv_tx_finish 最多會寫幾個取樣。 */
int nbtv_tx_tail_samples(const nbtv_tx *tx);

/*
 * 語音與畫面共用 FM 語音通道（說明在這個檔案最後一節）：打開之後，這個發射機給的是「畫面那一半」——
 * 放慢、移好位置、音量也照比例調好的取樣；外殼再把「語音那一半」（一個只推語音的 nbtv_duplex_tx 取出來的）
 * 一個取樣一個取樣加上去，就是送給電台的音訊。這樣畫面可以照原來的做法提早排好，語音到要送的那一刻才加。
 * 發射機要用 48000 建立。layout 是 NBTV_DUPLEX_STANDARD 或 NBTV_DUPLEX_NARROW；NBTV_DUPLEX_OFF 關掉（預設）。
 * voice_gain_db 是語音比畫面大幾 dB，要和語音那一半的 nbtv_duplex_tx_set_voice_gain_db 給同一個值。
 * 打開時每一幀在空中的時間是原來的 nbtv_duplex_slow_factor 倍：nbtv_tx_frame_samples 跟著變大
 * （它是上限，實際寫入的個數可能少幾個，以 nbtv_tx_encode_image 的回傳值為準）。
 * 只能在兩次發射之間改（nbtv_tx_finish 之後、下一次編碼之前）。成功回傳 0；layout 不對、發射機不是 48 kHz 的回傳 -1。
 */
#define NBTV_DUPLEX_OFF (-1)
int nbtv_tx_set_duplex(nbtv_tx *tx, int layout, float voice_gain_db);

/* ------------------------------------------------------------------ 接收 */

typedef struct nbtv_rx nbtv_rx;

/* sample_rate 是 12000 或 48000；其他值回傳 NULL。 */
nbtv_rx *nbtv_rx_create(int sample_rate);
void nbtv_rx_destroy(nbtv_rx *rx);

/* 清掉所有狀態，回到搜尋同步訊號（例如發射結束、重新開始接收時）。設定過的模型會留著。 */
void nbtv_rx_reset(nbtv_rx *rx);

/*
 * 各個模式用的模型。和發射機一樣可以設定好幾個（每個模式用實數個數、寬度與畫面大小都和它相同的那一個；三者都相同的取代前一個），
 * model 是 NULL 表示全部拿掉。某個模式沒有模型、或對方用的是另一份權重時，
 * 那個模式的幀照樣會回報（呼號、訊雜比都有），但沒有畫面：decoded = 0、model_missing = 1。
 */
void nbtv_rx_set_model(nbtv_rx *rx, const nbtv_model *model);

/*
 * 合併最近幾幀用的權重。可以設定好幾個（每個畫面模型各一份；同一個模型的取代前一個），merge 是 NULL 表示全部拿掉。
 * 設定了還要用 nbtv_rx_set_merging 打開才會用。某個模式的模型沒有對應的權重時，那個模式照原來的方式解
 * （192×144 的大畫面沒有這種權重）。
 */
void nbtv_rx_set_merge(nbtv_rx *rx, const nbtv_merge *merge);

/* 打開（非 0）或關掉（0）合併最近幾幀；預設是關的。nbtv_rx_reset 不會改變這個設定，只會忘掉前面收到的幀。 */
void nbtv_rx_set_merging(nbtv_rx *rx, int enabled);

/*
 * 打開（非 0，預設）或關掉（0）解畫面。關掉時幀照常接收、回報（呼號、幀序號、訊雜比都有），只跳過解畫面那一步：
 * 回報的幀 decoded = 0、skipped = 1。給外殼在來不及的時候用：解畫面是最花時間的一步，電腦太慢、一幀的時間裡解不完時，
 * 還沒處理的音訊會愈積愈多，畫面愈來愈晚出來。積太多的時候先關掉、消化完再打開，只少幾張畫面，同步不會斷。
 * 合併最近幾幀照樣把略過的那一幀記下來。隨時可以改，下一幀起生效；nbtv_rx_reset 不會改變這個設定。
 */
void nbtv_rx_set_decode_pictures(nbtv_rx *rx, int enabled);

/*
 * 送進來的音訊在別處被整段往下移過頻率時，告訴接收機移了多少 Hz（以 12 kHz 的取樣為準）；預設 0：沒有移過。
 * 給「語音與畫面共用 FM 語音通道」用：nbtv_duplex_rx 分出來的畫面訊號要設成 nbtv_duplex_band_shift_hz 的值，
 * 不然兩邊音效卡的取樣時脈差到一百多 ppm 時，沒有散佈導頻的幀（scattered_pilots = 0）會收不到。
 * 平常的收訊不要設。nbtv_rx_reset 不會改變這個設定。
 */
void nbtv_rx_set_band_shift_hz(nbtv_rx *rx, double hz);

/*
 * 接收「語音與畫面共用 FM 語音通道」的畫面（說明在這個檔案最後一節）：打開之後，推進來的音訊先分出畫面那一段、
 * 還原成原來的樣子才交給接收機，nbtv_rx_set_band_shift_hz 也跟著設好。接收機要用 48000 建立。
 * 語音那一段不在這裡處理（另外用 nbtv_duplex_rx 取出來交給語音的解碼器）。
 * 打開之後只收得到用同一種版面送的訊號，平常的訊號收不到：兩種都要收就各用一個接收機，推同樣的音訊。
 * 回報的幀在空中實際花的時間是 frame_seconds 的 nbtv_duplex_slow_factor 倍；音量的讀數是畫面那一段的。
 * layout 是 NBTV_DUPLEX_STANDARD 或 NBTV_DUPLEX_NARROW；NBTV_DUPLEX_OFF 關掉（預設）。切換時接收機回到初始狀態。
 * 成功回傳 0；layout 不對、接收機不是 48 kHz 的回傳 -1。
 */
int nbtv_rx_set_duplex(nbtv_rx *rx, int layout);

/*
 * 推入收到的音訊。同步、解調與解碼都在這個呼叫裡完成才返回：一幀結束時要解一張畫面，
 * 視模式與電腦的快慢要幾毫秒到幾百毫秒（會用到幾條執行緒，見「運算」一節）。
 */
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
    int frames_skipped;       /* 累計因為關掉解畫面而略過的幀數（nbtv_rx_set_decode_pictures） */
} nbtv_rx_status;

void nbtv_rx_get_status(const nbtv_rx *rx, nbtv_rx_status *out);

typedef struct nbtv_rx_frame {
    int mode_id;
    int decoded;         /* 1：有畫面，rgb 有效；0：這一幀沒有畫面 */
    int header_ok;       /* 呼號與幀序號可信（標頭可能壞掉而畫面仍在） */
    char callsign[NBTV_CALLSIGN_MAX + 1];
    int sequence;        /* 幀序號 0..255；header_ok 為 0 時是 -1 */
    float snr_db;
    float freq_offset_hz;
    int image_width;     /* 這個模式的畫面大小（像素）；rgb 的前 image_width × image_height × 3 個位元組是畫面 */
    int image_height;
    uint8_t rgb[NBTV_IMAGE_MAX_RGB_BYTES];
    int model_missing;   /* 收到了一幀，但沒有設定這個模式的模型、或權重和對方不同，所以沒有畫面 */
    int skipped;         /* 收到了一幀，但當時關掉了解畫面（nbtv_rx_set_decode_pictures），所以沒有畫面 */
} nbtv_rx_frame;

/*
 * 取出一個已經處理完的幀。有的話填入 out 並回傳 1，沒有回傳 0。
 * 有畫面的幀（decoded = 1）一定會回報。沒有畫面的幀（decoded = 0）只有在確定對方送了一整幀時才回報：
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

/* ------------------------------------------------------------------ 語音與畫面共用 FM 語音通道（實驗性） */

/*
 * 把 FM 語音通道的音訊頻帶切成兩段，同時送即時的語音與畫面：
 *   語音  外面的語音編解碼器給的訊號（8 kHz 的複數、中心 1500 Hz、約 860 Hz 寬：FreeDV 的 RADE V2 就是這種格式），
 *         放在 360–1220 Hz。核心不含語音的編解碼器，只負責把它放進去、拿出來。
 *   畫面  數據機原樣，但放慢播放、再往上移：下緣在 1335 Hz。任何模式都可以；每一幀在空中的時間是原來的
 *         nbtv_duplex_slow_factor 倍。
 * 只適合 FM（沒有頻偏、通帶平）。兩種用法：
 *   分開做  發射機用 12000 建立，它給的取樣交給 nbtv_duplex_tx_push_picture，語音的訊號交給 nbtv_duplex_tx_push_voice，
 *           再用 nbtv_duplex_tx_read 取出送給電台的音訊（48 kHz）。接收端把收到的音訊（48 kHz）交給 nbtv_duplex_rx_push，
 *           取出畫面那一路交給用 12000 建立、設定過 nbtv_rx_set_band_shift_hz 的接收機，語音那一路交給語音的解碼器。
 *           命令列工具 nbtv-duplex 是這樣做的。
 *   即時用  發射機（48000）打開 nbtv_tx_set_duplex，它給的就是畫面那一半；語音那一半用一個只推語音的 nbtv_duplex_tx，
 *           播放時兩者相加。接收機（48000）打開 nbtv_rx_set_duplex 直接收畫面；語音用一個關掉畫面那一路的 nbtv_duplex_rx。
 * 每一個物件同一時間只能給一條執行緒用。
 */
#define NBTV_DUPLEX_STANDARD 0 /* 畫面放慢 1.5 倍，佔 1335–2866 Hz */
#define NBTV_DUPLEX_NARROW 1   /* 畫面放慢 1.8 倍，只佔到 2611 Hz：給音訊上緣比較窄的電台 */

#define NBTV_DUPLEX_RATE 48000         /* 合成之後、分離之前的音訊 */
#define NBTV_DUPLEX_PICTURE_RATE 12000 /* 畫面那一路：發射機與接收機都用這個取樣率建立 */
#define NBTV_DUPLEX_VOICE_RATE 8000    /* 語音那一路：複數，I、Q 交錯 */

/* 版面的數字；layout 不是上面兩個值時都回傳 0。 */
double nbtv_duplex_slow_factor(int layout);     /* 畫面放慢幾倍 */
double nbtv_duplex_picture_low_hz(int layout);  /* 畫面在空中佔的頻率範圍 */
double nbtv_duplex_picture_high_hz(int layout);
double nbtv_duplex_voice_low_hz(int layout); /* 語音在空中佔的頻率範圍 */
double nbtv_duplex_voice_high_hz(int layout);
double nbtv_duplex_band_shift_hz(int layout); /* 給 nbtv_rx_set_band_shift_hz 的值 */

typedef struct nbtv_duplex_tx nbtv_duplex_tx;

nbtv_duplex_tx *nbtv_duplex_tx_create(int layout); /* layout 不對回傳 NULL */
void nbtv_duplex_tx_destroy(nbtv_duplex_tx *tx);
/* 清掉濾波器的狀態與還沒取走的音訊（一次發射結束、下一次開始之前）。語音的音量設定留著。 */
void nbtv_duplex_tx_reset(nbtv_duplex_tx *tx);
/* 語音比畫面大幾 dB：預設 0（兩者功率相同），範圍 −20…+12。兩路的比例跟著改，合起來的峰值維持在滿刻度以內。 */
void nbtv_duplex_tx_set_voice_gain_db(nbtv_duplex_tx *tx, float db);
/* 畫面的數據機訊號：用 12000 建立的發射機給的取樣（照預設的峰均比）。 */
void nbtv_duplex_tx_push_picture(nbtv_duplex_tx *tx, const float *samples, int count);
/* 語音的訊號：count 個複數（iq 有 2 × count 個 float），8 kHz、中心 1500 Hz，均方根約 0.7（RADE V2 給的大小）。 */
void nbtv_duplex_tx_push_voice(nbtv_duplex_tx *tx, const float *iq, int count);
/* 兩路各自處理好、還沒被取走的輸出取樣數（48 kHz）。 */
int nbtv_duplex_tx_pending_picture(const nbtv_duplex_tx *tx);
int nbtv_duplex_tx_pending_voice(const nbtv_duplex_tx *tx);
/* 取出 count 個送給電台的取樣（48 kHz），一定填滿：哪一路沒有東西就當成靜音。峰值不超過 ±1。 */
void nbtv_duplex_tx_read(nbtv_duplex_tx *tx, float *samples, int count);

typedef struct nbtv_duplex_rx nbtv_duplex_rx;

nbtv_duplex_rx *nbtv_duplex_rx_create(int layout);
void nbtv_duplex_rx_destroy(nbtv_duplex_rx *rx);
void nbtv_duplex_rx_reset(nbtv_duplex_rx *rx);
/* 只要其中一路時把另一路關掉（給 0），省下它的運算；預設兩路都做。關掉的那一路不會有東西可以取。 */
void nbtv_duplex_rx_enable(nbtv_duplex_rx *rx, int picture, int voice);
/* 電台收到的音訊（48 kHz）。 */
void nbtv_duplex_rx_push(nbtv_duplex_rx *rx, const float *samples, int count);
/* 分好、還沒被取走的取樣數（語音以複數的個數計）。 */
int nbtv_duplex_rx_available_picture(const nbtv_duplex_rx *rx);
int nbtv_duplex_rx_available_voice(const nbtv_duplex_rx *rx);
/* 取出畫面那一路（12 kHz 的取樣），最多 capacity 個；回傳取了幾個。 */
int nbtv_duplex_rx_read_picture(nbtv_duplex_rx *rx, float *samples, int capacity);
/* 取出語音那一路（8 kHz 的複數、中心 1500 Hz），最多 capacity 個複數（iq 要有 2 × capacity 個 float）；回傳取了幾個複數。 */
int nbtv_duplex_rx_read_voice(nbtv_duplex_rx *rx, float *iq, int capacity);

#ifdef __cplusplus
}
#endif

#endif /* NBTV_H */
