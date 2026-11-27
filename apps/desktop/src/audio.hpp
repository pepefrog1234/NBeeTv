// 音訊裝置（SDL3）：列出裝置、從指定的裝置收音、把排好的取樣送到指定的裝置。對應 macOS 版的 AudioDevices.swift 與 AudioIO.swift。
// 和核心之間一律是 48 kHz、單聲道、float；裝置自己的格式與取樣率由 SDL 轉換。
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nbtvgui {

struct AudioDevice {
    std::string name;
    SDL_AudioDeviceID id = 0;   // 只在這一次列出來的結果裡有效；設定裡記的是名稱
};

/// 系統上的音訊裝置。要先 SDL_InitSubSystem(SDL_INIT_AUDIO)。
std::vector<AudioDevice> audio_inputs();
std::vector<AudioDevice> audio_outputs();

/// 設定裡記的那個裝置。name 是空字串表示用系統預設的；指定的名稱現在不在（被拔掉了）時回傳空的，
/// 不會改用別的裝置——發射的聲音送錯地方比送不出去糟。
std::optional<AudioDevice> resolve_audio_device(const std::string& name, bool input);

/// 從一個輸入裝置收音。多聲道的裝置只取第一個聲道。
class AudioInput {
public:
    ~AudioInput();

    /// 收到的取樣（48 kHz）。在 SDL 的音訊執行緒上呼叫，不要在裡面做花時間的事。開始之前設定好。
    std::function<void(std::vector<float>)> on_samples;

    bool start(SDL_AudioDeviceID device, std::string* error);
    void stop();
    bool running() const { return stream_ != nullptr; }

private:
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
    SDL_AudioStream* stream_ = nullptr;
    int channels_ = 1;
    std::vector<float> scratch_;
};

/// 發射用的輸出：外面把取樣排進佇列（enqueue），裝置來取多少就給多少；佇列空了給靜音。
/// 「位置」都是從這一次 start 算起、排進佇列的第幾個取樣。
class AudioOutput {
public:
    explicit AudioOutput(double queue_seconds);
    ~AudioOutput();

    /// 輸出音量（線性的倍數），隨時可以改，下一批取樣就生效。
    std::atomic<float> gain{1.0f};

    // 測試用：不開音訊裝置，改由一條執行緒照實際的時間把取樣取走。取走的取樣交給 simulated_sink（在那條執行緒上呼叫）。
    // simulated_stall_after：取了這麼多秒之後就不再取，用來重現輸出裝置在發射中沒有回應。
    bool simulated = false;
    std::function<void(const std::vector<float>&)> simulated_sink;
    std::optional<double> simulated_stall_after;

    /// 佇列裡至少有這麼多個取樣才開始播（剛開始、或中途空掉之後）；0 是有多少播多少。
    /// 一小段一小段送來的即時聲音（解出來的語音）用它先墊一點，才不會斷斷續續。要在 start 之前設定。
    std::size_t start_level = 0;

    // 語音＋畫面：語音那一半另外排一條佇列，播放時一個取樣一個取樣加到主佇列的那一半上。
    // 畫面可以提早好幾秒排好，語音到要送的那一刻才編得出來，所以兩者不能排在同一條佇列裡。
    /// 語音的佇列墊到這麼多個取樣才開始加（語音是一段一段編出來的）；中途空掉之後也一樣。要在 start 之前設定。
    std::size_t voice_start_level = 0;
    /// 語音的佇列中途空掉（電腦一時忙不過來，語音還沒編出來）之後，要等到停的長度是這麼多個取樣的整數倍才接回去；
    /// 0 是墊夠了就接。設成語音的編解碼器要的那個單位（voice::gap_samples：RADE V2 是一個符號，V1 是一個數據幀）：這樣對方的接收端不必重新同步，
    /// 不然每停一次對方約半秒聽不到，剛好停在結尾前的話連「結束」的標記也收不到。要在 start 之前設定。
    std::size_t voice_gap_step = 0;
    /// 語音中途停下來、後來又接回去的次數（除錯用）。
    int voice_gaps() const;
    /// 把實際交給裝置的取樣（兩半加起來、還沒乘輸出音量）留一份，給發射時的頻譜顯示用（take_rendered）。要在 start 之前設定。
    bool keep_rendered = false;
    /// 把語音那一半排進它自己的佇列；放不下的部分丟掉（語音晚了就不送，不能愈積愈多）。
    void enqueue_voice(const float* samples, std::size_t count);
    /// 語音的佇列裡還有幾個取樣沒有播。
    std::int64_t voice_queued_samples() const;
    /// 取出從上次到現在實際交給裝置的取樣（keep_rendered 打開時才有）。
    std::vector<float> take_rendered();

    /// 開始輸出：佇列與計數都清掉。simulated 時 device 不用。
    bool start(SDL_AudioDeviceID device, std::string* error);
    void stop();
    bool running() const { return stream_ != nullptr || simulating_.load(); }

    /// 排進佇列。放不下時回傳 false（什麼都沒排）。
    bool enqueue(const float* samples, std::size_t count);
    bool enqueue(const std::vector<float>& samples) { return enqueue(samples.data(), samples.size()); }
    /// 把位置 from 之後排著的都拿掉；已經播到 from 之後了就不動、回傳 false。
    bool discard_queued(std::int64_t from);

    std::int64_t played_samples() const;    // 佇列裡的取樣已經被裝置取走幾個
    std::int64_t queued_samples() const;    // 還排著幾個
    std::int64_t rendered_frames() const;   // 裝置總共來取了幾格（含靜音）；不再增加就表示裝置沒有回應了
    std::int64_t silent_frames() const;     // 其中有幾格是佇列空了補的靜音
    int underruns() const;                  // 佇列從有東西變成空的次數

private:
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
    /// 取 count 個取樣到 out（乘上音量）；不夠的補靜音。
    void render(float* out, std::size_t count);
    void reset();

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<float> ring_;
    std::int64_t enqueued_ = 0;
    std::int64_t played_ = 0;
    std::int64_t rendered_ = 0;
    std::int64_t silent_ = 0;
    int underruns_ = 0;
    bool flowing_ = false;            // 主佇列已經墊夠、正在播
    std::size_t waited_ = 0;          // 佇列裡有東西、但還沒墊夠，已經等了幾個取樣
    std::deque<float> voice_;         // 語音那一半
    bool voice_flowing_ = false;
    std::size_t voice_waited_ = 0;
    bool voice_begun_ = false;        // 這一次（start 之後）語音開始送過了
    std::size_t voice_gap_ = 0;       // 語音停下來到現在幾個取樣了
    int voice_gaps_ = 0;
    std::vector<float> rendered_tap_; // keep_rendered：交給裝置的取樣
    SDL_AudioStream* stream_ = nullptr;
    std::vector<float> scratch_;
    std::thread simulator_;
    std::atomic<bool> simulating_{false};
};

}  // namespace nbtvgui
