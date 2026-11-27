// 語音與畫面共用 FM 語音通道（實驗性）：一邊送畫面，一邊送即時的語音。對應 macOS 版的 Voice.swift。
// 通道的音訊頻帶切成兩段，語音在低的一段（360–1220 Hz），畫面放慢之後搬到高的一段（1335 Hz 起）。
// 語音的編解碼器是 FreeDV 的 RADE V2（核心之外的 voice 模組）；建置時沒有把它編進來的話，voice::available() 是 false，
// 介面上不會出現這個功能。合成與分離由核心做（nbtv.h 的 nbtv_duplex_*）。
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio.hpp"
#include "core_bridge.hpp"
#include "tasks.hpp"

struct nbtv_voice_tx;
struct nbtv_voice_rx;
struct nbtv_duplex_tx;
struct nbtv_duplex_rx;

namespace nbtvgui {

namespace voice {
/// 這份程式有沒有把語音的編解碼器編進來
bool available();
/// 編解碼器的名稱與版本，例如「FreeDV RADE V2 (rade_c 23c3746)」
std::string codec_name();
/// 這個模式能不能和語音一起送：只有給 FM 用的模式（沒有導頻的，或大畫面的）
bool supports(const ModeInfo* mode);
/// 一幀在空中的時間是原來的幾倍
double slow_factor(VoiceLayout layout);
/// 放慢最多的那一種是幾倍（輸出佇列照它配置）
double slowest_factor();
/// 語音排進輸出佇列之後，墊到這麼多才開始送（語音是一段一段編出來的，每段 40 毫秒）
inline constexpr double kStartSeconds = 0.12;
/// 喇叭那一頭：解出來的語音墊到這麼多才開始播，積到上限就先不排新的（兩邊音效卡的快慢不會剛好一樣）
inline constexpr double kSpeakerStartSeconds = 0.15;
inline constexpr double kSpeakerLimitSeconds = 0.8;
/// 和語音一起送時，結尾留這麼久的靜音：語音的「結束」標記要在放開 PTT 之前送完
inline constexpr double kEndSeconds = 0.45;
}  // namespace voice

/// 發射端：麥克風的語音（48 kHz）→「語音那一半」的音訊（48 kHz，已經搬到語音的頻段、照比例調好音量），
/// 播放時一個取樣一個取樣加到畫面那一半上（AudioOutput::enqueue_voice）。只能在同一條執行緒上用。
class VoiceSender {
public:
    /// 編解碼器開不起來時回傳空的。
    static std::unique_ptr<VoiceSender> create(VoiceLayout layout);
    ~VoiceSender();
    VoiceSender(const VoiceSender&) = delete;
    VoiceSender& operator=(const VoiceSender&) = delete;

    /// 推入語音，回傳多出來的那一段音訊（還沒湊滿一段時是空的）。
    std::vector<float> process(const float* speech, int count);
    /// 這一次發射的語音結束：送出剩下的，加上「結束」的標記（對方的接收端看到它就知道講完了），並把濾波器裡的尾巴擠出來。
    std::vector<float> finish();

private:
    VoiceSender(nbtv_voice_tx* encoder, nbtv_duplex_tx* mixer);
    std::vector<float> drain();
    std::vector<float> take();
    nbtv_voice_tx* encoder_;
    nbtv_duplex_tx* mixer_;
    std::vector<float> iq_;
};

/// 接收端：電台收到的音訊（48 kHz）→ 分出語音那一段 → 解出語音（48 kHz）。只能在同一條執行緒上用。
class VoiceReceiver {
public:
    static std::unique_ptr<VoiceReceiver> create(VoiceLayout layout);
    ~VoiceReceiver();
    VoiceReceiver(const VoiceReceiver&) = delete;
    VoiceReceiver& operator=(const VoiceReceiver&) = delete;

    VoiceLayout layout() const { return layout_; }
    /// 回到剛建立的狀態（重新開始接收時）。
    void reset();
    /// 推入收到的音訊，回傳解出來的語音（沒有同步到語音的訊號時是空的）。
    std::vector<float> process(const float* samples, int count);

    struct Status {
        bool synced = false;   // 目前是不是同步到語音的訊號
        double snr_db = 0;     // 訊雜比的估計
        int overs_ended = 0;   // 到目前為止收到幾次「結束」的標記
    };
    Status status() const;

private:
    VoiceReceiver(VoiceLayout layout, nbtv_duplex_rx* splitter, nbtv_voice_rx* decoder);
    VoiceLayout layout_;
    nbtv_duplex_rx* splitter_;
    nbtv_voice_rx* decoder_;
    std::vector<float> iq_;
    std::vector<float> speech_;
};

/// 發射時語音走的那一條路：麥克風 → 編碼 → 排進輸出的語音佇列。編碼在自己的執行緒上做。
class VoiceTransmitPath {
public:
    VoiceTransmitPath();
    ~VoiceTransmitPath();

    /// 最近一小段麥克風的音量（均方根，dBFS）；還沒有聲音時是 −120。
    double level_db() const { return level_.load(); }

    /// 開始。microphone 是空的時不開任何裝置（測試用：語音由 speak 給）。開不了時回傳 false，這條路不會啟動。
    /// output 要活到 stop 之後。
    bool start(VoiceLayout layout, std::optional<SDL_AudioDeviceID> microphone, AudioOutput* output, std::string* error);
    /// 測試用：當成麥克風收到了這一段語音（48 kHz）。
    void speak(std::vector<float> samples);
    /// 這一次發射快結束了：不再收麥克風，把剩下的語音與「結束」的標記排進去。
    void finish();
    /// 立刻停下來（發射結束或中止）。
    void stop();

private:
    void feed(const std::vector<float>& samples);

    AudioInput microphone_;
    std::atomic<double> level_{-120.0};
    // 以下只在 worker_ 上讀寫
    std::unique_ptr<VoiceSender> sender_;
    AudioOutput* output_ = nullptr;
    bool active_ = false;
    Worker worker_;   // 宣告在最後：解構時最先結束
};

}  // namespace nbtvgui
