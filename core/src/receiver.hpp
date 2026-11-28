// 接收機：12 kHz 的實數音訊進，解出來的幀出。
// 內部流程：解析濾波 → 搜尋 S0（前後兩半相同）→ 以 S0、S1 判定頻偏與模式 → 收滿一幀後
// 解調、通道估測、解碼 → 依幀長預測下一幀的位置並持續追蹤。
// 收訊時也一邊繼續找前導符號：對方可能中途停了、換成另一次發射；失去同步時回頭從最後確定有東西的位置重新搜尋。
// push() 會在呼叫端的執行緒上把能做的事做完才返回（一幀收完時解畫面的那一段會另外用幾條執行緒一起算，見 parallel.hpp）。
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "chanest.hpp"
#include "dsp.hpp"
#include "learned_frame.hpp"
#include "learned_merge.hpp"
#include "modes.hpp"
#include "types.hpp"

namespace nbtv {

struct RxFrame {
    int mode_id = -1;
    bool decoded = false;        // 有畫面（rgb 有效）
    bool model_missing = false;  // 收到了一幀，但沒有設定這個模式的模型，或對方用的是另一份權重，所以沒有畫面
    bool skipped = false;        // 收到了一幀、也解得出來，但設定成先不解畫面（RxTuning::skip_pictures），所以沒有畫面
    bool header_ok = false;      // 標頭（呼號、幀序號）可信。標頭壞了畫面仍可能還原得出來
    std::string callsign;
    int sequence = -1;
    float snr_db = 0.0f;
    float freq_offset_hz = 0.0f;
    int image_width = 0;            // 這個模式的畫面大小（像素）：模式 3 與 11–14 是 96×72，模式 4–7 與 15 是 192×144
    int image_height = 0;
    std::vector<std::uint8_t> rgb;  // 寬×高×3 的畫面，decoded 時有效
    std::int64_t start_sample = 0;  // 幀開頭在輸入串流裡的位置（12 kHz 的取樣編號）
};

struct RxStatus {
    bool synced = false;
    int mode_id = -1;              // 未同步時是 -1
    float snr_db = 0.0f;           // synced 且 snr_valid 時有效
    bool snr_valid = false;
    float freq_offset_hz = 0.0f;
    int symbols_done = 0;          // 目前這一幀已經收到的符號數（含 S0、S1）
    int symbols_total = 0;
    float sync_quality = 0.0f;     // 最近一次前導符號的分數（只有雜訊時約為 1，愈大愈確定）
    float input_level_db = -120.0f;  // 輸入音量（均方根，dBFS）
    long frames_decoded = 0;
    long frames_failed = 0;
    long frames_skipped = 0;       // 設定成先不解畫面而略過的幀數
};

// 同步判定的參數。預設值由模擬定出；公開出來是為了讓模擬工具可以掃描。
struct RxTuning {
    // S0 滑動相關 |P|²/(E1·E2) 的門檻。取得很低：只有雜訊時也常常超過，但後面還有兩道檢查，
    // 這一關只是挑出值得檢查的位置；−5 dB 的 S0 仍有九成會通過。
    double sc_threshold = 0.02;
    int detect_window = 32;       // 前導符號判定假設的延遲擴散（取樣數，2.7 ms）
    int detect_first = -40;       // 捕獲時延遲視窗起點相對於 FFT 視窗起點的搜尋範圍（追蹤時定時已知，範圍較窄）
    int detect_last = 72;
    // 分數的定義見 check_preamble。只有雜訊時，45 個假設（9 個模式 × 5 個整數頻偏）裡最大的分數
    // 中位數約 2.2、約 2% 超過 3.0；−3 dB 的訊號中位數約 5.9、−5 dB 約 4.3。
    // （這是只比 9 條 S1 序列時量的。現在 16 條序列全部都比、共 80 個假設，其中模式表裡沒有的編號只用來
    // 認出「別的模式」、不會被收下來；門檻沒有跟著調，後面還有導頻驗證把關：四小時的純雜訊沒有一次進入同步。）
    double acquire_score = 3.0;   // 捕獲的門檻（通過後還要經過導頻驗證）
    // 追蹤時確認下一幀前導符號的門檻（只檢查目前的模式）。只在上一幀解出來時使用；
    // 上一幀沒解出來的話改用 acquire_score，而且同樣要經過導頻驗證。
    double track_score = 2.4;
    // 追蹤時換到另一個模式的門檻。上一幀沒解出來的話要 2 × acquire_score。
    double switch_score = 3.6;
    // 鎖定之後照幀的節拍接著收：上一幀的導頻從頭到尾都很清楚的話，下一幀就算前導符號沒確認到、標頭也沒解出來，
    // 只要它的導頻同樣清楚、S1 也和目前的模式對得上，就照樣當成一幀（Receiver::process_frame）。
    // 訊號弱時最先撐不住的是標頭與前導符號，畫面其實還解得出來；少了這個，連續發射在 3 kHz 訊雜比 −4 dB 時
    // 只交得出三成的幀。關掉時每一幀都要靠自己的標頭或前導符號（比較用）。
    bool follow = true;
    // 捕獲後的第二道驗證：用 S1 估出的通道預測酬載開頭 12 個符號的導頻，與收到的值的正規化相關
    // （同時搜尋 ±16 Hz 內的殘餘頻偏）。只有雜訊或對錯位置時約 0.2，−3 dB 的訊號約 0.65。
    double verify_pilots = 0.40;
    // 沒有散佈導頻的幀（Mode::scattered_pilots 為 false）沒有導頻可以驗證，改看酬載開頭 12 個符號的循環字首
    // 和符號的尾巴是不是相同（Receiver::cyclic_coherence）：只有雜訊時約 0.1，訊雜比 0 dB 的訊號約 0.5。
    double verify_cyclic = 0.40;
    // 有散佈導頻的幀：幀尾附近有很像 S0、還來不及判定的東西時，這一幀不算完整（Receiver::process_frame）。這是原來的規則，
    // 現在預設關掉——酬載自己常常就像 S0，短波的錄音裡一成到六成五的幀因此不算完整，標頭沒解出來時整張就不交出去。
    // 打開是比較用。
    bool tail_candidate_veto = false;
    // 一幀收到一半，音訊斷開過（音訊在送到發射機的路上卡了一下，訊號停幾毫秒到幾十毫秒再原樣接續；或少了一段取樣，
    // 例如送給網路 SDR 電台的音訊封包沒到）：斷開之後的符號整批晚到或早到，照原來的對時，那一幀不是沒了就是壞一截。
    // 打開時，沒解出來的幀先記著；下一幀的前導符號如果出現在「不是整數幀」的位置，差的那幾個取樣就是斷開的長度，
    // 回頭把那一幀分成前後兩段各自對時，再解一次（Receiver::mend_frame）。解出來了但可疑的幀（標頭沒解出來、有一段沒有導頻、
    // 導頻中間有幾對對不上）先留著不交，等下一幀的位置確定：就在原來的節拍上的話照原樣交（晚 0.07 到 0.4 秒），不在的話
    // 補救之後再交；一次發射的最後一幀沒有下一幀可以等，最多晚 0.7 秒。訊號乾淨、標頭也正確的幀不會被留。
    // 只管有散佈導頻的幀；關掉是比較用。
    bool mend_steps = true;
    // 是否使用逐符號、逐載波的雜訊估計（見 ChannelEstimate::noise_at）。關掉時每個格點都用同一個雜訊變異數。
    bool noise_map = true;
    // 有散佈導頻的幀：頻帶兩端的載波明顯比一般的乾淨時，照量到的給（ChannelEstimator::estimate 的 1b）。有別的訊號疊在頻帶中間時
    // 才用得上——語音疊在畫面上（duplex.hpp 的「疊加」）就是：大部分的載波被語音蓋住，兩端蓋不到的其實乾淨得多，
    // 照一般的雜訊算就太看輕它們，標頭常常因此解不出來。對方有沒有疊語音是從收到的幀自己看出來的，不必另外告訴接收機；
    // 平常的訊號（各載波的雜訊差不多）不受影響。關掉是比較用。
    bool quiet_carriers = true;
    // 沒有散佈導頻的幀：同一個符號裡各個載波的雜訊不一樣大時，照量到的給（見 Receiver::process_frame_free「各載波的雜訊」）。
    // FM 的訊號弱到門檻以下時，雜訊集中在低頻；差多少每一台接收機不一樣，所以是從收到的幀自己量，沒有照哪一台定的數字。
    // 關掉時同一個符號裡每個載波都用同一個雜訊變異數（比較用）。
    bool carrier_noise = true;
    // 找前導符號、確認訊號在不在之前，先把輸入的頻譜拉平（Receiver::flatten）。FM 的訊號弱的時候雜訊集中在低頻，
    // 這些判定用的相關值都被低頻的雜訊拖累；拉平之後才是它們設計時假設的白雜訊。拉多少只看輸入自己的頻譜重心，
    // 沒有照哪一台電台定的數字；輸入本來就平（短波、訊號夠強的 FM）時不動。只影響「找」，解碼用的還是原來的取樣。
    bool flatten_search = true;
    // 各格點雜訊不一樣大時，交給解碼器之前的處理方式，見 LearnedRxOptions（learned_frame.hpp）。
    bool learned_median_snr = true;
    bool learned_attenuate = true;
    bool learned_floor = true;
    // 解這一幀之前，把前面幾幀收到的值合進來（learned_merge.hpp）：訊號弱、畫面平靜時畫面比較清楚也比較穩定。
    // 要另外設定合併用的權重（Receiver::set_merge）；這一幀的模型沒有對應的權重時，和關掉一樣。
    bool learned_merge = false;
    // 先不解畫面：幀照常接收、回報（標頭、訊雜比都有），只跳過最花時間的那一步，回報的幀 skipped 是 true。
    // 給呼叫端在來不及的時候用（音訊積太多時先打開，消化完再關掉）：只少幾張畫面，同步與追蹤都不中斷。
    // 合併最近幾幀照樣把略過的那一幀收到的值記下來。隨時可以改，下一幀起生效。
    bool skip_pictures = false;
    // 送進來的音訊在別處被整段往下移過頻率時（語音與畫面共用 FM 語音通道的用法，見 duplex.hpp），移了多少 Hz
    // （以這裡的 12 kHz 取樣為準）；0 表示沒有移過。只影響沒有散佈導頻的幀：兩邊音效卡的取樣時脈差一點時，
    // 移過頻率的訊號除了一個符號比一個符號晚到，整個頻帶還會一起慢慢轉，轉的角度和晚到的取樣數成正比，
    // 比例就是這個值（見 Receiver::process_frame_free）。有散佈導頻的幀本來就會由導頻量出殘餘的頻偏，不需要它。
    double band_shift_hz = 0.0;
};

class Receiver {
public:
    Receiver();

    RxTuning& tuning() { return tuning_; }

    // 各個模式用的模型。每個模式用「實數個數、寬度與畫面大小都和它相同」的那一個，所以要收幾個模式就設定幾個；
    // 三者都相同的再設定一次會取代前一個，傳空指標表示全部拿掉。某個模式沒有模型時，它的幀只回報
    // 「收到一幀，但沒有畫面」。reset() 不會清掉模型。
    void set_model(std::shared_ptr<const LearnedCodec> model) { models_.set(std::move(model)); }

    // 合併最近幾幀用的權重，每個畫面模型各一份（以它記的模型識別碼對應）。同一個模型的再設定一次會取代前一個，
    // 傳空指標表示全部拿掉。要 tuning().learned_merge 打開才會用。reset() 不會清掉權重，只清掉記下來的前幾幀。
    void set_merge(std::shared_ptr<const LearnedMerge> merge);

    void reset();
    void push(std::span<const float> samples);  // 12 kHz
    bool poll(RxFrame& out);                    // 取出一個已完成的幀
    RxStatus status() const;
    // 音訊到此為止（讀完一個檔案）：有一幀留著、等下一幀的位置確定才交的話（RxTuning::mend_steps），現在照原樣交出去，
    // 之後用 poll 取。即時收訊不必呼叫——後面一直有音訊進來，留著的幀最多 0.7 秒就會交出來。之後還可以繼續 push。
    void finish();

    // 目前已經處理到的輸入取樣數
    std::int64_t samples_received() const { return total_in_; }

    // 把同步與解碼的過程印到 stderr（除錯與命令列工具用）
    void set_trace(bool on) { trace_ = on; }

private:
    enum class State { Search, Receive, Confirm };

    struct PreambleCheck {
        int mode_id = -1;         // 最像的模式編號（0..15）；可能是模式表裡沒有的，那種幀不收
        int shift = 0;            // 整數頻偏（FFT 格數）
        double score = 0.0;       // 最佳假設的分數：延遲視窗內外的能量密度比，只有雜訊時約為 1
        double score_s1 = 0.0;    // 同一個假設只看 S1 的分數
        double score_mode = 0.0;  // 指定模式（追蹤時是目前的模式）在 shift = 0 的分數
    };

    std::int64_t end() const { return base_ + static_cast<std::int64_t>(buf_.size()); }
    const cf& sample(std::int64_t index) const { return buf_[static_cast<std::size_t>(index - base_)]; }
    const cf& flat_sample(std::int64_t index) const { return flat_[static_cast<std::size_t>(index - base_)]; }
    void flatten(std::size_t from);

    // 捕獲到的一幀：前導符號通過了 try_acquire 的兩道檢查（前導符號的分數、酬載開頭的導頻）
    struct Acquisition {
        const Mode* mode = nullptr;
        double cfo = 0.0;      // 頻偏，每個取樣的週數（已經加上導頻驗證時估出的修正）
        std::int64_t t0 = 0;   // S0 循環字首的起點
        double score = 0.0;    // 前導符號的分數
        double metric = 0.0;   // S0 滑動相關的值，用來在解出第一幀之前粗估訊雜比
    };

    bool step();
    bool search_step();
    bool try_acquire(std::int64_t s0_start, cd corr, double metric);
    void begin_frame(const Acquisition& found);
    void fft_window(std::int64_t start, double cfo, cf* out, bool flat = false) const;
    PreambleCheck check_preamble(std::int64_t s0_window, double cfo, std::span<const int> shifts, int expect_mode, int detect_first,
                                 int detect_last) const;
    struct PathDelays {
        double first = 0.0;  // 第一條有份量的路徑
        double peak = 0.0;   // 最強的路徑
    };
    PathDelays s1_path_delays(std::int64_t s1_window, double cfo, int mode_id) const;
    SymbolGrid demodulate(std::int64_t t0, double cfo, int rows, bool flat = false) const;
    bool pilots_present(std::int64_t t0, double cfo, const Mode& mode) const;
    struct PilotCheck {
        double rho = 0.0;        // 正規化相關
        double cfo_delta = 0.0;  // 殘餘頻偏（每個取樣的週數）
    };
    PilotCheck verify_pilots(std::int64_t t0, double cfo, const Mode& mode) const;
    void process_frame();
    bool mend_frame(const Acquisition& next);
    void release_held();
    void drop_mend();
    void decode_picture(const Mode& mode, const LearnedUnpacked& l, bool whole, RxFrame& frame);
    // 沒有散佈導頻的幀（給 FM 語音通道用）走的路，對應上面靠導頻的那幾個：
    double cyclic_coherence(std::int64_t t0, int first_row, int last_row) const;
    bool cyclic_signal(std::int64_t symbol_start, double& power) const;
    bool carrier_shape(int k, const std::array<cf, kCarriers>& channel, double channel_power, std::array<float, kCarriers>& out,
                       double& tilt_db) const;
    bool signal_present(std::int64_t t0, const Mode& mode) const;
    PilotCheck verify_frame_start(std::int64_t t0, double cfo, const Mode& mode) const;
    void null_powers(std::int64_t t0, int rows, std::vector<float>& out) const;
    void process_frame_free();
    const LearnedMerge* find_merge(std::uint32_t model_id) const;
    // 把前面幾幀合進這一幀。回傳用了幾幀；0 表示沒有可以用的（merged 不能用）。
    int merge_earlier(const LearnedMerge& merge, const Mode& mode, const LearnedUnpacked& frame, const RxFrame& info,
                      LearnedUnpacked& merged) const;
    void remember(const LearnedMerge& merge, const Mode& mode, const LearnedUnpacked& frame, const RxFrame& info);
    void confirm_next();
    void enter_search(std::int64_t from);
    void start_scan(std::int64_t from, std::int64_t limit);
    // 目前這一幀前導符號的證據夠不夠強；不夠的話是暫定的幀，一邊收一邊找到更確定的前導符號就改收那一幀
    bool tentative() const { return frame_evidence_ < 2.0 * tuning_.acquire_score; }
    void trim();

    RxTuning tuning_;
    LearnedModels models_;
    std::vector<std::shared_ptr<const LearnedMerge>> merges_;
    // 最近解出畫面的幾幀收到的值（新的在前），合併用。只在合併打開、而且那一幀的模型有合併用的權重時才記。
    struct Earlier {
        int mode_id = -1;
        std::uint32_t model_id = 0;
        std::int64_t start = 0;  // 幀開頭的取樣編號
        bool header_ok = false;
        int sequence = -1;
        std::string callsign;
        std::vector<float> values;
        std::vector<float> noise_var;
    };
    std::deque<Earlier> earlier_;
    AnalyticFilter analytic_;
    ChannelEstimator chanest_;
    std::vector<float> clean_;   // push 進來的取樣整理後暫放的地方（非有限值換成 0）
    // 還沒濾波的取樣，和 buf_ 用同樣的編號（buf_ 的第 i 個是它在 i − analytic_.delay_samples() 附近的值）。
    // 解析濾波器只留 220–2780 Hz；沒有散佈導頻的幀要聽頻帶上方（到 4 kHz）的雜訊，得從這裡取。
    std::vector<float> raw_;
    std::vector<cf> buf_;
    // buf_ 經過拉平頻譜的一階濾波器之後的取樣，同樣的編號：flat_[n] = buf_[n] − a·buf_[n−1]（見 flatten）。
    // 找前導符號、確認訊號在不在的那幾步讀這一份；a 是 0 的時候和 buf_ 完全相同。
    std::vector<cf> flat_;
    double flat_a_ = 0.0;                      // 目前的 a
    double flat_r0_ = 0.0;                     // 輸入最近約一秒的自相關：延遲 0、1、2 個取樣
    cd flat_r1_{0.0, 0.0}, flat_r2_{0.0, 0.0};
    cf flat_prev_{0.0f, 0.0f}, flat_prev2_{0.0f, 0.0f};  // 前一個、前兩個取樣
    int flat_count_ = 0;                       // 離上一次重新算 a 過了幾個取樣
    std::int64_t base_ = 0;      // buf_[0] 的取樣編號
    std::int64_t total_in_ = 0;
    double level_ = 0.0;         // 輸入平方值的指數平均

    State state_ = State::Search;

    // 搜尋 S0 用的滑動相關
    std::int64_t scan_pos_ = 0;
    std::int64_t scan_limit_ = INT64_MAX;  // 只找到這個位置之前
    bool sc_valid_ = false;
    int sc_age_ = 0;
    cd sc_p_{0, 0};
    double sc_e1_ = 0.0, sc_e2_ = 0.0;
    cd sc_background_{0, 0};  // 相關值的長時間平均：固定的載波干擾會讓相關值一直偏向某個方向，要先扣掉
    bool peak_active_ = false;
    double peak_metric_ = 0.0;
    std::int64_t peak_pos_ = 0;
    cd peak_p_{0, 0};
    bool cand_ready_ = false;
    std::int64_t cand_pos_ = 0;
    cd cand_p_{0, 0};
    double cand_metric_ = 0.0;

    // 追蹤中的訊號
    const Mode* mode_ = nullptr;
    double cfo_ = 0.0;           // 頻偏，每個取樣的週數
    std::int64_t t0_ = 0;        // 目前這一幀 S0 循環字首的起點（第一條路徑）
    bool confirmed_ = false;     // 這一幀的前導符號有沒有確認到
    bool presence_checked_ = false;
    bool last_ok_ = false;       // 上一幀有沒有解出來（標頭正確，或是交出去了而且導頻從頭到尾都很清楚）
    bool last_complete_ = false; // 上一幀解出來了，而且導頻一直到幀尾都在（對方把整幀送完了）
    // 照節拍接著收（RxTuning::follow）：上一幀的導頻從頭到尾都很清楚，下一幀可以不靠前導符號與標頭接著收。
    bool last_clear_ = false;
    int follow_misses_ = 0;      // 接著收的幀連續幾次 S1 沒對上（容許一次）
    // 失去同步時要從哪裡重新搜尋：最近一個「確定有東西」的位置之後。解出一幀之後是那一幀的結尾，
    // 捕獲或確認到前導符號之後是那個前導符號之後。這樣一來，沒解出來的幀所佔的時間會再搜尋一次——
    // 對方可能中途停了、另一次發射就在那段時間裡開始。
    std::int64_t rescan_from_ = 0;
    int unconfirmed_run_ = 0;
    double frame_evidence_ = 0.0;  // 目前這一幀前導符號的分數；沒確認到前導符號的幀是 0
    // 目前這一幀（前導符號很明確）還沒收完，裡面就出現另一個很明確的前導符號時，那個前導符號的位置；沒有是 −1。
    // pending_ 是它驗證過的結果：目前這一幀收完之後直接從那裡收下去，不回頭再搜尋一次。
    std::int64_t pending_start_ = -1;
    Acquisition pending_;
    // 最近一個處理完的幀（前導符號確認過的）在捕獲或確認時的起點。起點沒有超過它的幀不再捕獲一次：
    // 處理過的幀只會愈來愈後面，所以不管輸入是什麼，push() 一定會返回。
    std::int64_t done_start_ = INT64_MIN;
    double sync_quality_ = 0.0;
    float snr_db_ = 0.0f;
    bool snr_valid_ = false;
    // 最近一個沒有交出去、或解出來了但留著還沒交的幀（有散佈導頻的）：它可能是中途斷開了（RxTuning::mend_steps）。
    // 緩衝區留著它的取樣，等下一幀的前導符號出現（begin_frame）再決定要不要回頭補救；確認到下一幀就在原來的節拍上、
    // 或等太久，就作廢（留著的照原樣交出去）。
    struct Mend {
        bool valid = false;
        const Mode* mode = nullptr;
        std::int64_t t0 = 0;    // 那一幀的起點與頻偏（第一次處理時用的）
        double cfo = 0.0;
        bool trusted = false;   // 它的前導符號確認過，而且很明確或緊接在一張好的後面：補救之後標頭還是沒解出來時，導頻清楚也算數
        // 第一次解出來了、但可疑的幀：結果（還沒解成畫面）留在這裡沒有交出去。補救成了就換成補救的，不成就照原樣交（release_held）。
        bool held = false;
        RxFrame frame;
        LearnedUnpacked unpacked;
    };
    Mend mend_;
    // 兩邊的取樣時脈差多少：照節拍接下來的幀，定時每一幀修正了幾個取樣（除以幀長；最近幾幀的加權和與權重）。
    // 下一幀的起點本來就會因此偏幾個取樣，mend_frame 算斷開的長度時要扣掉。
    double trend_sum_ = 0.0, trend_weight_ = 0.0;
    bool from_previous_ = false;        // 目前這一幀的起點是由上一幀的起點加一幀的長度來的（不是剛捕獲的）
    const Mode* last_mode_ = nullptr;   // 最近處理完的那一幀的模式與結尾（下一幀照節拍該開始的地方）
    std::int64_t last_end_ = 0;

    // 沒有散佈導頻的幀：同一次發射（一幀接一幀）之間留著的東西。FM 的音訊通道整次發射都不變，
    // 所以通道的估計可以跨幀平均；取樣時脈差多少也不會變。捕獲到新的一次發射時清掉（begin_frame）。
    struct FreeTrack {
        static constexpr int kNullBins = 28;        // 頻帶上方沒用到的頻率：第 58–85 格（2719–3984 Hz）
        int frames = 0;                             // ref 是幾幀平均出來的；0 表示還沒有（這次發射的第一幀）
        std::array<cf, kCarriers> ref{};            // 平均的通道（含參考符號的振幅），對齊到最近一幀開頭的參考符號
        double ref_var = 0.0;                       // ref 每個載波的誤差變異數（估出來的；平均時照它決定前幾幀佔多少份量）
        // 從最近一幀開頭的參考符號到下一幀開頭的，訊號預計再晚幾個取樣（已經扣掉視窗跟著移動的整數部分）、相位再轉多少
        double pending_shift = 0.0;
        double pending_phase = 0.0;
        double drift = 0.0;                         // 時脈漂移：每過一個符號，訊號比照著幀長算的位置晚到幾個取樣（最近幾幀的平均）
        int drift_count = 0;                        // drift 是幾幀平均出來的
        double fraction = 0.0;                      // 幀起點還沒算進去的小數部分（取樣數）
        // 每個符號的雜訊要用的（見 process_frame_free「每個符號的雜訊」）：最近幾幀在頻帶上方各格聽到的能量，
        // 以及那幾幀的參考符號——頻帶內量到的雜訊，和同一個符號在頻帶上方的能量。只留最近幾幀
        // （和通道平均的幀數一樣多）：訊號由弱轉強或由強轉弱之後，「平常有多吵」和兩邊怎麼換算都要跟著變。
        static constexpr int kNoiseFrames = 8;
        struct NoiseFrame {
            std::array<double, kNullBins> null_sum{};             // 這一幀所有符號在頻帶上方各格的能量合計
            int rows = 0;                                         // 合計了幾個符號
            int references = 0;                                   // 量到雜訊的參考符號有幾個（0–2）
            std::array<double, 2> noise{};                        // 它們各自在頻帶內量到的雜訊變異數
            std::array<std::array<float, kNullBins>, 2> nulls{};  // 它們各自在頻帶上方各格的能量
        };
        std::array<NoiseFrame, kNoiseFrames> noise_frames{};
        int noise_count = 0;  // 留著幾幀
        int noise_next = 0;   // 下一幀寫在哪一個位置
        float noise_var = 0.0f;                     // 最近一次量到的雜訊變異數（這一幀沒有參考符號可以量時沿用）
        // 各載波的雜訊形狀：符號照吵雜的程度分成幾級，各級分開累計（見 process_frame_free「各載波的雜訊」）。
        // 每過一幀，之前累計的份量會打折，所以是最近幾幀的加權和。
        static constexpr int kShapeClasses = 4;
        std::array<std::array<double, kCarriers>, kShapeClasses> shape_power{};  // 每個載波收到的功率
        std::array<double, kShapeClasses> shape_signal{};                        // 其中訊號佔的（所有載波合計）
        std::array<double, kShapeClasses> shape_rows{};                          // 累計了幾個符號
    };
    FreeTrack free_;

    bool trace_ = false;
    std::deque<RxFrame> frames_;
    long frames_decoded_ = 0;
    long frames_failed_ = 0;
    long frames_skipped_ = 0;
};

}  // namespace nbtv
