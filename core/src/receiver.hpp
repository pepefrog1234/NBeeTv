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
    int image_width = 0;            // 這個模式的畫面大小（像素）：模式 11–14 是 96×72，模式 5–7 與 15 是 192×144
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
    // 是否使用逐符號、逐載波的雜訊估計（見 ChannelEstimate::noise_at）。關掉時每個格點都用同一個雜訊變異數。
    bool noise_map = true;
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
    void fft_window(std::int64_t start, double cfo, cf* out) const;
    PreambleCheck check_preamble(std::int64_t s0_window, double cfo, std::span<const int> shifts, int expect_mode, int detect_first,
                                 int detect_last) const;
    struct PathDelays {
        double first = 0.0;  // 第一條有份量的路徑
        double peak = 0.0;   // 最強的路徑
    };
    PathDelays s1_path_delays(std::int64_t s1_window, double cfo, int mode_id) const;
    SymbolGrid demodulate(std::int64_t t0, double cfo, int rows) const;
    bool pilots_present(std::int64_t t0, double cfo, const Mode& mode) const;
    struct PilotCheck {
        double rho = 0.0;        // 正規化相關
        double cfo_delta = 0.0;  // 殘餘頻偏（每個取樣的週數）
    };
    PilotCheck verify_pilots(std::int64_t t0, double cfo, const Mode& mode) const;
    void process_frame();
    void decode_picture(const Mode& mode, const LearnedUnpacked& l, bool whole, RxFrame& frame);
    // 沒有散佈導頻的幀（給 FM 語音通道用）走的路，對應上面靠導頻的那幾個：
    double cyclic_coherence(std::int64_t t0, int first_row, int last_row) const;
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

    // 沒有散佈導頻的幀：同一次發射（一幀接一幀）之間留著的東西。FM 的音訊通道整次發射都不變，
    // 所以通道的估計可以跨幀平均；取樣時脈差多少也不會變。捕獲到新的一次發射時清掉（begin_frame）。
    struct FreeTrack {
        static constexpr int kNullBins = 28;        // 頻帶上方沒用到的頻率：第 58–85 格（2719–3984 Hz）
        int frames = 0;                             // ref 是幾幀平均出來的；0 表示還沒有（這次發射的第一幀）
        std::array<cf, kCarriers> ref{};            // 平均的通道（含參考符號的振幅），對齊到最近一幀開頭的參考符號
        // 從最近一幀開頭的參考符號到下一幀開頭的，訊號預計再晚幾個取樣（已經扣掉視窗跟著移動的整數部分）、相位再轉多少
        double pending_shift = 0.0;
        double pending_phase = 0.0;
        double drift = 0.0;                         // 時脈漂移：每過一個符號，訊號比照著幀長算的位置晚到幾個取樣（最近幾幀的平均）
        int drift_count = 0;                        // drift 是幾幀平均出來的
        double fraction = 0.0;                      // 幀起點還沒算進去的小數部分（取樣數）
        std::array<double, kNullBins> null_sum{};   // 頻帶外各格的能量總和與列數：各格平常有多吵
        long null_rows = 0;
        double scale_num = 0.0, scale_den = 0.0;    // 頻帶外的相對大小換算成頻帶內雜訊變異數的係數（分子、分母）
        float noise_var = 0.0f;                     // 最近一次量到的雜訊變異數（這一幀沒有參考符號可以量時沿用）
    };
    FreeTrack free_;

    bool trace_ = false;
    std::deque<RxFrame> frames_;
    long frames_decoded_ = 0;
    long frames_failed_ = 0;
    long frames_skipped_ = 0;
};

}  // namespace nbtv
