// 短波通道模擬（12 kHz 的實數音訊進出），測試與模擬工具使用，不屬於核心函式庫。
//   * Watterson 模型：兩條等強的路徑，各自做瑞利衰落，都卜勒頻譜是高斯形
//   * 頻率偏移（SSB 收發機沒對準）、接收端取樣時脈誤差
//   * 白雜訊：訊雜比以 3 kHz 雜訊頻寬計
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "rng.hpp"
#include "types.hpp"

namespace nbtv::sim {

struct ChannelParams {
    bool fading = false;        // false：單一路徑、不衰落
    double delay_ms = 0.0;      // 第二條路徑的延遲
    double doppler_hz = 0.0;    // 每條路徑的都卜勒擴散（2σ）
    bool add_noise = true;
    double snr_db = 30.0;       // 訊號平均功率 ÷（N0 × 3 kHz）
    double freq_offset_hz = 0.0;
    double clock_ppm = 0.0;     // 正值表示接收端的取樣時脈偏快
    // 干擾（功率都相對於訊號的平均功率，dB）
    double tone_hz = 0.0;       // 單頻載波的頻率；0 表示沒有
    double tone_db = 0.0;
    double impulse_rate = 0.0;  // 每秒幾次脈衝雜訊；0 表示沒有
    double impulse_db = 20.0;   // 脈衝期間的雜訊功率
    double impulse_ms = 20.0;   // 每次持續多久
    std::uint64_t seed = 1;
};

// 草案與 ITU-R F.1487 用的幾個通道：awgn、poor（2 ms／1 Hz）、moderate（1 ms／0.5 Hz）、slow（0.5 ms／0.1 Hz）。
// 名稱不認得時回傳 false。
bool channel_preset(std::string_view name, ChannelParams& params);

class Channel {
public:
    // signal_power 是輸入訊號的平均功率（均方值），用來換算雜訊的大小。
    Channel(const ChannelParams& params, double signal_power);

    // 串流處理：輸出接在 out 後面。因為濾波器延遲與時脈誤差，輸出的個數不一定等於輸入。
    void process(std::span<const float> in, std::vector<float>& out);

    // 目前兩條路徑的增益（量測用）
    cd path_gain(int path) const { return gain_now_[path]; }

private:
    struct Fader {
        std::vector<double> taps;     // 高斯形低通
        std::vector<cd> history;      // 低取樣率的複數高斯雜訊
        std::size_t pos = 0;
        cd current{1, 0}, next{1, 0}; // 相鄰兩個低取樣率的增益，線性內插
        cd filter() const;
    };

    void advance_faders();
    void emit(cd z);
    void resample_flush(std::vector<float>& out);

    ChannelParams params_;
    Rng rng_;
    double noise_sigma_ = 0.0;

    // 希爾伯特轉換：實數 → 解析訊號
    std::vector<double> hilbert_;
    std::vector<double> real_hist_;
    std::size_t real_pos_ = 0;

    // 第二條路徑的延遲線（解析訊號）
    std::vector<cd> delay_line_;
    std::size_t delay_pos_ = 0;
    int delay_samples_ = 0;

    Fader fader_[2];
    int fade_phase_ = 0;      // 兩次衰落更新之間的位置
    int fade_interval_ = 0;   // 每幾個取樣更新一次衰落增益
    cd gain_now_[2];

    double offset_phase_ = 0.0;
    double signal_power_ = 0.0;
    double tone_phase_ = 0.0;
    int impulse_left_ = 0;  // 目前這次脈衝還剩幾個取樣
    double interference();  // 下一個取樣的干擾值

    // 時脈誤差：以加窗 sinc 內插重取樣
    std::vector<double> rs_hist_;
    std::size_t rs_count_ = 0;   // 已收到的（重取樣前）取樣數
    double rs_next_ = 0.0;       // 下一個輸出取樣在輸入時間軸上的位置
    std::vector<double> pending_;
};

}  // namespace nbtv::sim
