// 語音與畫面共用一個 FM 語音通道（實驗性）：把通道的音訊頻帶切成兩段，語音用低的一段、畫面用高的一段。
//
//   語音   外面的語音編解碼器給的訊號（8 kHz 的複數、中心在 1500 Hz、約 860 Hz 寬：FreeDV 的 RADE V2 就是這種格式）。
//          移到中心 790 Hz、只留 360–1220 Hz。核心不含語音的編解碼器，只負責把它放進去、拿出來。
//   畫面   數據機原樣（12 kHz 的取樣），但放慢 k 倍播放：頻寬變成 1/k，每一幀的時間變成 k 倍。再往上移，下緣放在 1335 Hz。
//          k = 1.5（標準）時畫面佔 1335–2866 Hz；k = 1.8（窄）時只佔到 2611 Hz，給音訊上緣比較窄的電台。
//
// 兩者相加之後就是送進電台麥克風端的音訊（48 kHz）。接收端反過來：把兩段分開，畫面的那一段還原成 12 kHz 的取樣交給
// 原來的接收機，語音的那一段還原成 8 kHz 的複數交給語音的解碼器。數據機、幀的格式、模型都不必改。
//
// 接收機要知道一件事：畫面被移過頻率。兩邊音效卡的取樣時脈差一點時，移過頻率的訊號整個頻帶會一起慢慢轉，
// 沒有散佈導頻的幀要靠這個數字才接得起來（RxTuning::band_shift_hz，給 duplex_band_shift_hz 的值）。
//
// 這種用法只適合 FM：通道沒有頻偏（兩段都是照絕對的音訊頻率放的），而且通帶夠平。
// 來歷與模擬的結果見 docs/TECHNICAL.md「語音與畫面共用 FM 語音通道」。都是串流式的：一次可以給任意長度。
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "types.hpp"

namespace nbtv {

enum class DuplexLayout {
    Standard = 0,  // 畫面放慢 1.5 倍，佔 1335–2866 Hz
    Narrow = 1,    // 畫面放慢 1.8 倍，佔 1335–2611 Hz
};

inline constexpr int kDuplexRate = 48000;         // 進出電台的音訊取樣率
inline constexpr int kDuplexPictureRate = 12000;  // 畫面數據機的取樣率（放慢之前）
inline constexpr int kDuplexVoiceRate = 8000;     // 語音訊號的取樣率

inline constexpr double kVoiceInputCentre = 1500.0;  // 語音訊號交進來、交出去時的中心頻率（Hz）
inline constexpr double kVoiceCentre = 790.0;        // 它在空中的中心
inline constexpr double kVoiceHalfBand = 430.0;      // 只留中心 ± 這麼多（360–1220 Hz）
inline constexpr double kVoiceStopBand = 545.0;      // 離中心這麼遠的地方（畫面的下緣）要壓乾淨
inline constexpr double kPictureLow = 1335.0;        // 畫面在空中的下緣
inline constexpr double kModemLow = 352.0;           // 數據機佔的頻率範圍（12 kHz 取樣時）：第 8–56 格再多半格
inline constexpr double kModemHigh = 2648.0;

// 語音訊號交進來時的大小：複數的均方根（RADE V2 給的就是這麼大）。兩路相加的比例照這個數字算。
inline constexpr double kVoiceNominalRms = 0.696;

// 畫面放慢的倍數：一幀的時間變成原來的這麼多倍。
constexpr double duplex_slow_factor(DuplexLayout layout) { return layout == DuplexLayout::Narrow ? 1.8 : 1.5; }
// 畫面在空中佔的頻率範圍的上緣（Hz）；下緣是 kPictureLow。
constexpr double duplex_picture_high(DuplexLayout layout) {
    return kPictureLow + (kModemHigh - kModemLow) / duplex_slow_factor(layout);
}
// 接收端把畫面搬回原位時往下移了多少（以還原之後的 12 kHz 取樣為準的 Hz）：給接收機的 RxTuning::band_shift_hz。
constexpr double duplex_band_shift_hz(DuplexLayout layout) { return kPictureLow * duplex_slow_factor(layout) - kModemLow; }

namespace duplex_detail {

// 取樣率乘上 up/down 的重取樣（多相 FIR）。pass、stop 是通帶與阻帶的邊緣（Hz），input_rate 是輸入的取樣率。
class Resampler {
public:
    Resampler(int up, int down, double input_rate, double pass_hz, double stop_hz);
    void reset();
    // 輸出接在 out 後面。
    void process(std::span<const float> in, std::vector<float>& out);

private:
    int up_, down_;
    std::size_t per_phase_;       // 每一相用幾個輸入取樣
    std::vector<float> taps_;     // 排成 [相][第幾個]，對應的輸入由舊到新
    std::vector<float> history_;  // 最近的輸入，存兩份（長度是 per_phase_ 的兩倍），取連續的一段時不必繞回開頭
    std::size_t pos_ = 0;
    int phase_ = 0;
};

// 把頻譜裡的一段搬到另一個位置：先把 from（以取樣率為 1 的頻率）移到 0，低通濾波只留那一段，再移到 to。
// 輸入可以是實數或複數；輸出是複數（解析訊號），要實數的話取實部。
class BandMover {
public:
    // pass、stop：低通的通帶與阻帶邊緣（以取樣率為 1）；attenuation_db：阻帶要壓多少
    BandMover(double from, double to, double pass, double stop, double attenuation_db);
    void reset();
    // 輸出接在 out 後面，和輸入一樣多。
    void process(std::span<const float> in, std::vector<cf>& out);
    void process(std::span<const cf> in, std::vector<cf>& out);
    int delay_samples() const { return static_cast<int>(taps_.size() - 1) / 2; }

private:
    cf step(cf mixed);
    double from_, to_;
    std::vector<float> taps_;
    std::vector<cf> history_;  // 存兩份，理由同上
    std::size_t pos_ = 0;
    double phase_in_ = 0.0, phase_out_ = 0.0;  // 兩個旋轉的相位（圈數的小數部分）
};

}  // namespace duplex_detail

// 發射端：畫面的數據機訊號與語音的訊號合成一路。
class DuplexTx {
public:
    explicit DuplexTx(DuplexLayout layout);

    DuplexLayout layout() const { return layout_; }
    // 語音比畫面大幾 dB（預設 0：兩者功率相同；範圍 −20…+12）。兩路的比例跟著改，合起來的峰值維持在滿刻度以內。
    void set_voice_gain_db(float db);
    void reset();

    // 畫面數據機的取樣（12 kHz，核心的發射機用 12000 建立、照預設的峰均比給的）。
    void push_picture(std::span<const float> samples);
    // 語音的訊號（8 kHz 的複數，中心 1500 Hz，均方根約 kVoiceNominalRms）。
    void push_voice(std::span<const cf> samples);
    // 還沒被 read 取走的輸出取樣數（48 kHz）。
    std::size_t pending_picture() const { return picture_.size() - picture_read_; }
    std::size_t pending_voice() const { return voice_.size() - voice_read_; }
    // 取出 out.size() 個送給電台的取樣（48 kHz），一定填滿：哪一路沒有東西就當成靜音。峰值不超過 ±1。
    // 兩路也可以各用一個物件做（一個只推畫面、一個只推語音，語音的音量設成一樣），取出來的相加就是合起來的訊號：
    // 畫面可以提早好幾秒排好，語音到要送的那一刻才加上去。
    void read(std::span<float> out);
    // 把濾波器裡還沒出來的畫面擠出來（推入一小段靜音）：一次發射的最後一幀之後呼叫，之後 pending_picture 就是剩下的尾巴。
    void flush_picture();

private:
    DuplexLayout layout_;
    float picture_gain_ = 0.0f, voice_gain_ = 0.0f;
    duplex_detail::BandMover picture_mover_;
    duplex_detail::Resampler picture_up_;
    duplex_detail::BandMover voice_mover_;
    duplex_detail::Resampler voice_up_;
    std::vector<cf> scratch_;
    std::vector<float> real_;
    std::vector<float> picture_, voice_;  // 已經處理好、等著被取走的（48 kHz）
    std::size_t picture_read_ = 0, voice_read_ = 0;
};

// 接收端：把電台收到的音訊分成畫面與語音兩路。
class DuplexRx {
public:
    explicit DuplexRx(DuplexLayout layout);

    DuplexLayout layout() const { return layout_; }
    void reset();
    // 只要其中一路時把另一路關掉，省下它的運算（預設兩路都做）。關掉的那一路不會有東西可以取。
    void enable(bool picture, bool voice) {
        want_picture_ = picture;
        want_voice_ = voice;
    }
    // 電台收到的音訊（48 kHz）。非有限值當成 0。
    void push(std::span<const float> samples);
    // 分好、還沒被取走的取樣數。
    std::size_t available_picture() const { return picture_.size() - picture_read_; }
    std::size_t available_voice() const { return voice_.size() - voice_read_; }
    // 取出畫面那一路（12 kHz 的取樣：交給用 12000 建立、設定了 band_shift_hz 的接收機）；回傳取了幾個（最多 out.size()）。
    std::size_t read_picture(std::span<float> out);
    // 取出語音那一路（8 kHz 的複數，中心 1500 Hz，大小和發射端交進去的差不多）；回傳取了幾個。
    std::size_t read_voice(std::span<cf> out);

private:
    DuplexLayout layout_;
    duplex_detail::Resampler picture_down_;
    duplex_detail::BandMover picture_mover_;
    duplex_detail::Resampler voice_down_;
    duplex_detail::BandMover voice_mover_;
    std::vector<float> clean_, real_;
    std::vector<cf> scratch_;
    std::vector<float> picture_;
    std::vector<cf> voice_;
    std::size_t picture_read_ = 0, voice_read_ = 0;
    bool want_picture_ = true, want_voice_ = true;
};

}  // namespace nbtv
