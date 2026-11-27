// 同時送語音（實驗性）：一邊送畫面，一邊送即時的語音。對應 macOS 版的 Voice.swift。兩種做法、三種版面：
//   標準、窄  給 FM 用的模式。通道的音訊頻帶切成兩段，語音在低的一段（360–1220 Hz），畫面放慢之後搬到高的一段（1335 Hz 起）。
//             語音的編解碼器是 FreeDV 的 RADE V2。
//   疊加      給短波的模式（SSB）。畫面原樣，FreeDV RADE V1 的語音原樣加在同一個頻帶上：兩邊互相當成雜訊，都比單獨送差，
//             但畫面不必放慢，而且和只用 FreeDV（聽語音）或沒有語音功能的 NBeeTv（收畫面）互通。
// 編解碼器在核心之外的 voice 模組；建置時沒有把它編進來的話，voice::available() 是 false，介面上不會出現這個功能。
// 合成與分離由核心做（nbtv.h 的 nbtv_duplex_*）。
#pragma once

#include <array>
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
/// 編解碼器的名稱與版本，例如「FreeDV RADE V1 + V2 (rade_c 23c3746)」
std::string codec_name();
/// 三種版面，照介面上的順序
inline constexpr std::array<VoiceLayout, 3> kLayouts{VoiceLayout::standard, VoiceLayout::narrow, VoiceLayout::overlay};
/// 介面上的名稱（標準、窄、疊加）
std::string layout_title(VoiceLayout layout);
/// 語音是直接疊在畫面上的（不是分頻）：畫面沒有動過，平常的接收機就收得到，不必另外用一個接收機
constexpr bool is_overlay(VoiceLayout layout) { return layout == VoiceLayout::overlay; }
/// 這個模式能不能照這個版面和語音一起送：分頻的兩種只配給 FM 用的模式（沒有導頻的，或大畫面的），疊加只配短波的模式
bool supports(const ModeInfo* mode, VoiceLayout layout);
/// 一幀在空中的時間是原來的幾倍（疊加是 1：沒有放慢）
double slow_factor(VoiceLayout layout);
/// 放慢最多的那一種是幾倍（輸出佇列照它配置）
double slowest_factor();
/// 語音的佇列中途空掉之後，輸出要等到停的長度是這麼多個取樣（48 kHz）的整數倍才接回去，對方的接收端才不必重新同步
/// （見 AudioOutput::voice_gap_step）。RADE V2 是一個符號（960 個，20 毫秒），V1 是一個數據幀（5760 個，120 毫秒）；
/// 沒有編解碼器時是 0。
int gap_samples(VoiceLayout layout);
/// 語音排進輸出佇列之後，墊到這麼多才開始送。語音是一段一段編出來的：RADE V2 每段 40 毫秒（墊三段），V1 每段 120 毫秒（墊兩段）
constexpr double start_seconds(VoiceLayout layout) { return is_overlay(layout) ? 0.24 : 0.12; }
/// 和語音一起送時，結尾留這麼久的靜音：語音的「結束」標記要在放開 PTT 之前送完。
/// 要放得下墊著的那些、最後不滿一段補滿的、和標記本身（V2 是 120 毫秒，V1 是 144 毫秒）
constexpr double end_seconds(VoiceLayout layout) { return is_overlay(layout) ? 0.70 : 0.45; }
/// 喇叭那一頭：解出來的語音墊到這麼多才開始播，積到上限就先不排新的（兩邊音效卡的快慢不會剛好一樣）
inline constexpr double kSpeakerStartSeconds = 0.15;
inline constexpr double kSpeakerLimitSeconds = 0.8;
}  // namespace voice

/// 發射端：麥克風的語音（48 kHz）→「語音那一半」的音訊（48 kHz，已經放到語音該在的頻段、照比例調好音量），
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

/// 接收端：電台收到的音訊（48 kHz）→ 分出語音那一段（疊加的版面是整段音訊）→ 解出語音（48 kHz）。只能在同一條執行緒上用。
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
