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

inline constexpr int kImageWidth = NBTV_IMAGE_WIDTH;
inline constexpr int kImageHeight = NBTV_IMAGE_HEIGHT;
inline constexpr int kRgbBytes = NBTV_IMAGE_RGB_BYTES;
inline constexpr int kSpectrumBins = NBTV_SPECTRUM_BINS;   // 0–3000 Hz
inline constexpr double kSpectrumMaxHz = NBTV_SPECTRUM_MAX_HZ;
inline constexpr int kSampleRate = 48000;                   // 外殼與核心之間一律用 48 kHz
inline constexpr double kBandLowHz = 352.0;                 // 訊號頻帶
inline constexpr double kBandHighHz = 2648.0;

/// 一張 96×72 的畫面：RGB，每個像素三個位元組，由左上角開始逐列排列。
/// 桌面版只處理這個大小：畫面是 192×144 的模式 15（給 FM 用的）還沒有搬過來。
struct Picture {
    std::array<std::uint8_t, kRgbBytes> rgb{};
};
using PicturePtr = std::shared_ptr<const Picture>;

/// 把輸入整理成可以傳送的呼號（轉大寫、去掉不能用的字元、最多 9 個字元）。
std::string normalize_callsign(const std::string& text);

/// 各個模式用的模型與「合併最近幾幀」用的權重：從 models/ 載入一次（找的地方見 tools/model_file.hpp），之後大家共用。
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
    /// 第一次呼叫時載入（四份模型共約一百多 MB，要一秒上下）。
    static const Models& shared();

    const std::vector<LoadedModel>& models() const { return models_; }
    const std::vector<LoadedMerge>& merges() const { return merges_; }
    /// 「一張 96×72 的畫面變成 values 個實數、第一層 width 個通道」的那個模型；沒有的話是 nullptr
    const LoadedModel* find(int values, int width) const;
    /// 這個畫面模型（以識別碼表示）有沒有配合併用的權重
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
    std::string model_name;   // 用的模型的名稱，例如「261123-1」
    bool large_model = false; // 用的是加寬的大型網路（運算量約四倍）

    std::string name() const;          // 「模式 11」
    std::string quality() const;       // 快速／粗略／中等／細緻，依每幀的長短
    std::string label() const;         // 「模式 11 · 中等」
    std::string seconds_text() const;  // 「2.72 秒」
    std::string content_text() const;  // 「類神經網路」或「大型網路」
    std::string summary() const;       // 右側欄模式鈕下面的說明
};

namespace modes {
/// 有模型的模式，依核心模式表的順序。少了哪一份權重檔，那個模式就不在裡面（選單不列、也不能發射）。
const std::vector<ModeInfo>& all();
/// 選單的順序：依每幀的長短由快到慢；一樣長的，模型小的在前（模式編號是 14、12、11、13）
std::vector<ModeInfo> menu_order();
const ModeInfo* find(int id);
/// 沒有選過（或原本選的模式已經不存在）時用的模式：模式 11；沒有它的模型的話，用手上有的第一個。
int default_id();
}  // namespace modes

/// 接收機交出來的一幀。
struct DecodedFrame {
    int mode_id = 0;
    bool decoded = false;        // 有畫面
    bool model_missing = false;  // 收到了一幀，但權重和對方的不同（或沒有那個模式的權重檔），所以沒有畫面
    bool header_ok = false;
    std::string callsign;
    int sequence = -1;
    double snr_db = 0;
    double offset_hz = 0;
    PicturePtr picture;          // 有畫面時才有
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
    PicturePtr preview;          // 沒有雜訊時對方看到的畫面
    int mode_id = 0;
    int sequence = 0;
};

class CoreTransmitter {
public:
    explicit CoreTransmitter(int sample_rate = kSampleRate);
    ~CoreTransmitter();
    CoreTransmitter(const CoreTransmitter&) = delete;
    CoreTransmitter& operator=(const CoreTransmitter&) = delete;

    bool set_mode(int id);
    void set_callsign(const std::string& callsign);
    int frame_samples() const;
    /// 把一張畫面做成一幀音訊。失敗（目前的模式沒有模型）回傳空的。
    std::optional<EncodedFrame> encode(const Picture& picture);
    /// 取消最近編好、但決定不送的那一幀：發射機回到編它之前的狀態。沒有可以取消的幀時回傳 false。
    bool discard_last();
    /// 發射結束時取出重取樣濾波器裡剩下的尾巴。
    std::vector<float> finish();

private:
    nbtv_tx* handle_;
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
