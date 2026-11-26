// 核心 C API（nbtv.h）的 C++ 包裝，對應 macOS 版的 CoreBridge.swift。
// 這裡的物件都不是執行緒安全的：同一個物件只能在同一條執行緒（Worker）上使用，由呼叫端負責。
// 模型載入之後不會再改動，可以同時被不同執行緒上的發射機、接收機使用。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nbtv/nbtv.h"

namespace nbtvgui {

inline constexpr int kImageWidth = NBTV_IMAGE_WIDTH;     // 標準的畫面大小（模式 11–14）
inline constexpr int kImageHeight = NBTV_IMAGE_HEIGHT;
inline constexpr int kRgbBytes = NBTV_IMAGE_RGB_BYTES;
inline constexpr int kSpectrumBins = NBTV_SPECTRUM_BINS;   // 0–3000 Hz
inline constexpr double kSpectrumMaxHz = NBTV_SPECTRUM_MAX_HZ;
inline constexpr int kSampleRate = 48000;                   // 外殼與核心之間一律用 48 kHz
inline constexpr double kBandLowHz = 352.0;                 // 訊號頻帶
inline constexpr double kBandHighHz = 2648.0;

/// 畫面的大小（像素）。由模式決定：模式 11–14 是 96×72，模式 5–7 與 15（給 FM 用的大畫面）是 192×144。
struct PictureSize {
    int width = kImageWidth;
    int height = kImageHeight;

    /// 一張畫面的位元組數（8 位元 RGB）
    constexpr int rgb_bytes() const { return width * height * 3; }
    /// 相對於標準大小放大幾倍：96×72 是 1，192×144 是 2。畫布的字、內附的測試圖照這個倍數放大。
    constexpr int scale() const { return width / kImageWidth > 1 ? width / kImageWidth : 1; }
    /// 「192×144」
    std::string text() const { return std::to_string(width) + "×" + std::to_string(height); }
    constexpr bool operator==(const PictureSize& other) const { return width == other.width && height == other.height; }
    constexpr bool operator!=(const PictureSize& other) const { return !(*this == other); }
};
/// 模式 11–14 的畫面
inline constexpr PictureSize kStandardPicture{};

/// 一張畫面：RGB，每個像素三個位元組，由左上角開始逐列排列。大小跟著模式（沒有指定就是標準的 96×72）；
/// rgb 一定剛好是 size.rgb_bytes() 個位元組。
struct Picture {
    PictureSize size;
    std::vector<std::uint8_t> rgb;

    /// 每個值都是 level 的一張（預設全黑）。
    explicit Picture(PictureSize picture_size = kStandardPicture, std::uint8_t level = 0)
        : size(picture_size), rgb(static_cast<std::size_t>(picture_size.rgb_bytes()), level) {}
    int width() const { return size.width; }
    int height() const { return size.height; }
};
using PicturePtr = std::shared_ptr<const Picture>;

/// 把輸入整理成可以傳送的呼號（轉大寫、去掉不能用的字元、最多 9 個字元）。
std::string normalize_callsign(const std::string& text);

/// 各個模式用的模型與「合併最近幾幀」用的權重：從 models/ 載入一次，之後大家共用。
/// 哪個模式用哪一份權重檔，照命令列工具的那一份對照（tools/model_file.hpp 的 default_model_path、default_merge_path；
/// 找檔案的地方也見那裡），所以核心的模式表多了一個模式、那邊補上它的權重檔之後，這裡不必改就會載入。
/// 找不到或損壞的那一份不在裡面：對應的模式不會列在選單裡，收到那種幀也不會有畫面。
struct LoadedModel {
    std::string name;   // 模型的名稱（訓練完成的年月日-當天第幾個），例如「261123-1」
    nbtv_model* handle = nullptr;
};
struct LoadedMerge {
    std::string name;
    nbtv_merge* handle = nullptr;
};
class Models {
public:
    /// 第一次呼叫時載入（七份模型共約一百五十 MB，要一秒上下）。
    static const Models& shared();

    const std::vector<LoadedModel>& models() const { return models_; }
    const std::vector<LoadedMerge>& merges() const { return merges_; }
    /// 「一張 picture 大小的畫面變成 values 個實數、第一層 width 個通道」的那個模型
    /// （模式的 learned_values、learned_width 與畫面大小，三樣都要相同）；沒有的話是 nullptr
    const LoadedModel* find(int values, int width, PictureSize picture = kStandardPicture) const;
    /// 這個畫面模型（以識別碼表示）有沒有配合併用的權重（192×144 的大畫面沒有）
    bool merge_covers(std::uint32_t model_id) const;

private:
    Models();
    std::vector<LoadedModel> models_;
    std::vector<LoadedMerge> merges_;
};

/// 一個傳輸模式，資料取自 nbtv_mode_at()。
struct ModeInfo {
    int id = 0;
    double frame_seconds = 0;
    int frame_symbols = 0;
    int payload_symbols = 0;
    int learned_values = 0;   // 一張畫面變成幾個實數
    int learned_width = 0;    // 模型第一層的通道數
    PictureSize picture;      // 畫面的大小：96×72；模式 5–7 與 15 是 192×144
    bool scattered_pilots = true;   // 酬載裡有沒有散佈導頻；沒有的是給 FM 用的幀（模式 5–7），同樣的時間多送 2 成的資料
    std::string model_name;   // 用的模型的名稱，例如「261123-1」
    bool large_model = false; // 用的是加寬的大型網路（運算量約四倍）

    /// 畫面比標準的 96×72 大：給 VHF／UHF 的 FM 語音通道用的模式（模式 15，以及沒有導頻的模式 5–7）。
    /// 它的模型只照訊號好的情形訓練，訊號弱、有衰落時比其他模式差，不適合短波。
    bool large_picture() const { return picture != kStandardPicture; }
    /// 幀裡沒有導頻（模式 5–7）。這種幀只能用在訊號穩定、沒有頻偏的 FM 語音通道。
    bool pilot_free() const { return !scattered_pilots; }

    std::string name() const;          // 「模式 11」
    std::string quality() const;       // 快速／粗略／中等／細緻，依每幀的長短
    std::string detail() const;        // 「中等」；幀裡沒有導頻的模式是「FM 無導頻」，其餘的大畫面模式是「FM 大畫面」
    std::string menu_detail() const;   // 模式選單裡那一列的說明（見 core_bridge.cpp）
    std::string label() const;         // 「模式 11 · 中等」「模式 15 · FM 大畫面」
    std::string seconds_text() const;  // 「2.72 秒」
    std::string content_text() const;  // 「類神經網路」或「大型網路」；大畫面的模式是畫面的大小「192×144」
    std::string summary() const;       // 右側欄模式鈕下面的說明
};

namespace modes {
/// 有模型的模式，依核心模式表的順序。少了哪一份權重檔，那個模式就不在裡面（選單不列、也不能發射）。
const std::vector<ModeInfo>& all();
/// 選單的順序：依每幀的長短由快到慢；一樣長的，模型小的在前（模式編號是 14、12、11、13）。
/// 大畫面的模式是另外一類，排在後面：先是有導頻的（15），再是沒有導頻的（5、6、7，由快到慢）。
/// 選單裡各自成一組，見 menu_groups。
std::vector<ModeInfo> menu_order();
/// 模式選單的三組，各組裡照 menu_order 的順序；每個模式剛好在其中一組。空的那一組選單不畫。
/// 分組只看模式表裡的兩樣（畫面的大小、有沒有導頻），不看模式編號：核心的模式表多了模式，會自己落在該在的那一組。
struct MenuGroups {
    std::vector<ModeInfo> standard;     // 畫面是標準的 96×72
    std::vector<ModeInfo> large;        // 大畫面、幀裡有導頻（模式 15）
    std::vector<ModeInfo> pilot_free;   // 大畫面、幀裡沒有導頻（模式 5、6、7）
};
MenuGroups menu_groups();
/// 核心的模式表裡有、但這裡沒有模型（所以不在 all() 裡）的模式編號。權重檔都在的話是空的。
std::vector<int> without_model();
const ModeInfo* find(int id);
/// 沒有選過（或原本選的模式已經不存在）時用的模式：模式 11；沒有它的模型的話，用手上有的第一個。
int default_id();
}  // namespace modes

/// 接收機交出來的一幀。
struct DecodedFrame {
    int mode_id = 0;
    bool decoded = false;        // 有畫面
    bool model_missing = false;  // 收到了一幀，但權重和對方的不同（或沒有那個模式的權重檔），所以沒有畫面
    bool skipped = false;        // 收到了一幀，但那時候來不及解畫面（CoreReceiver::set_decode_pictures），所以沒有畫面
    bool header_ok = false;
    std::string callsign;
    int sequence = -1;
    double snr_db = 0;
    double offset_hz = 0;
    PicturePtr picture;          // 有畫面時才有；大小跟著那一幀的模式（96×72，模式 5–7 與 15 是 192×144）
};

class CoreReceiver {
public:
    explicit CoreReceiver(int sample_rate = kSampleRate);
    ~CoreReceiver();
    CoreReceiver(const CoreReceiver&) = delete;
    CoreReceiver& operator=(const CoreReceiver&) = delete;

    void reset();
    /// 要不要把最近幾幀合起來解（預設不要）。只對有配合併用權重的模型有作用；重設不會改變這個設定。
    void set_merging(bool on);
    /// 要不要解畫面（預設要）。關掉時幀照樣收、照樣回報，只是沒有畫面（DecodedFrame::skipped）：
    /// 電腦來不及的時候先關掉、消化完積著的音訊再打開，只少幾張畫面，同步不會斷。重設不會改變這個設定。
    void set_decode_pictures(bool on);
    void push(const float* samples, int count);
    nbtv_rx_status status() const;
    std::optional<DecodedFrame> poll_frame();

private:
    nbtv_rx* handle_;
    std::unique_ptr<nbtv_rx_frame> frame_;
};

/// 發射端做好的一幀。
struct EncodedFrame {
    std::vector<float> samples;  // 48 kHz
    PicturePtr preview;          // 沒有雜訊時對方看到的畫面，和送進去的那一張一樣大
    int mode_id = 0;
    int sequence = 0;
};

class CoreTransmitter {
public:
    explicit CoreTransmitter(int sample_rate = kSampleRate);
    ~CoreTransmitter();
    CoreTransmitter(const CoreTransmitter&) = delete;
    CoreTransmitter& operator=(const CoreTransmitter&) = delete;

    /// 換模式，下一幀生效。模式編號沒有定義時回傳 false，模式與畫面的大小都不變。
    bool set_mode(int id);
    void set_callsign(const std::string& callsign);
    int frame_samples() const;
    /// 目前這個模式的畫面大小：encode 只收這個大小的畫面
    PictureSize picture_size() const { return picture_size_; }
    /// 把一張畫面做成一幀音訊。畫面要是目前這個模式的大小（picture_size）。
    /// 失敗（大小不對、目前的模式沒有模型）回傳空的。
    std::optional<EncodedFrame> encode(const Picture& picture);
    /// 取消最近編好、但決定不送的那一幀：發射機回到編它之前的狀態。沒有可以取消的幀時回傳 false。
    bool discard_last();
    /// 發射結束時取出重取樣濾波器裡剩下的尾巴。
    std::vector<float> finish();

private:
    nbtv_tx* handle_;
    PictureSize picture_size_;
};

class CoreSpectrum {
public:
    explicit CoreSpectrum(int sample_rate = kSampleRate);
    ~CoreSpectrum();
    CoreSpectrum(const CoreSpectrum&) = delete;
    CoreSpectrum& operator=(const CoreSpectrum&) = delete;

    void reset();
    void push(const float* samples, int count);
    /// 取出一列頻譜（dBFS，257 點）；沒有的話回傳 false。
    bool poll(std::array<float, kSpectrumBins>& row);

private:
    nbtv_spectrum* handle_;
};

}  // namespace nbtvgui
