// 使用者設定：存成一個文字檔（每行「名稱=值」），放在系統給這個 app 的設定目錄裡。對應 macOS 版的 Settings.swift。
// 另外還有定時發射的時刻怎麼算（macOS 版的 Schedule.swift）。
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "ptt.hpp"
#include "sources.hpp"

namespace nbtvgui {

struct Settings {
    /// 我的呼號（寫進每張畫面的標頭）
    std::string callsign;
    /// 音訊裝置的名稱；空字串表示用系統預設
    std::string input_device;
    std::string output_device;
    /// 輸出音量（dB，−40…0）
    double output_level_db = -6;
    PTTMode ptt_mode = PTTMode::none;
    std::string serial_port;
    SerialLine serial_line = SerialLine::rts;
    std::string rig_host = "127.0.0.1";
    std::string rig_port = "4532";
    /// 用電台的 CAT 指令控制 PTT 時的電台型號：Hamlib 的型號編號（0：還沒選）與選的時候看到的名稱（「Icom IC-7300」）。
    /// 名稱另外存一份，沒有裝 Hamlib 的時候畫面上才寫得出選的是哪一部
    int rig_model = 0;
    std::string rig_model_name;
    /// CAT 的序列速率（bps）；0：用 Hamlib 替那個型號定的預設
    int serial_baud = 0;
    /// 發射模式的編號
    int tx_mode_id = 11;
    /// 發射來源。第一次啟動時是影片檔（還沒選檔）：不會一打開就把鏡頭開起來
    SourceKind source_kind = SourceKind::video;
    ScaleMode scale_mode = ScaleMode::fill;
    std::string camera_name;
    bool loop_video = true;
    /// 上次選的影片檔（或圖片）的路徑。下次啟動時自動開啟；定時發射播的就是這個檔案
    std::string video_path;
    /// 圖片序列：送完最後一張要不要從第一張再來。沒有勾（預設）就是每張送一次，送完自動停止發射
    bool loop_images = false;
    /// 圖片序列：上次選的那一批圖片的路徑，照發射的順序。下次啟動時自動帶回來
    std::vector<std::string> image_paths;
    /// 定時發射：每隔幾分鐘一次，對齊整點（15 就是每小時的 00、15、30、45 分）。開關本身不存：每次開 app 都是關的
    int schedule_interval_minutes = 15;
    /// 定時發射：到了時間正在收別人的畫面時，那一次不發射
    bool schedule_skip_when_busy = true;
    /// 在收到的畫面與縮圖的左上角標出那一張的訊雜比
    bool show_frame_snr = false;
    /// 把一段畫面存成影片時，在每一張的左上角印上收到它當下的訊雜比（按「儲存影片」時的選項，勾了會記住）
    bool stamp_video_snr = false;
    /// 畫布：送出去的每一張畫面上要不要印字、印什麼、印在上面還是下面
    bool canvas_enabled = false;
    std::string canvas_text;
    CanvasPosition canvas_position = CanvasPosition::top;
    /// 介面的語言；空字串表示跟著系統
    std::string language;
    /// 上次的視窗大小（0：用預設的）
    int window_width = 0;
    int window_height = 0;

    /// 測試用：有設定時，發射用它給的 PTT 驅動，不看 ptt_mode（自我測試用來確認 PTT 有按下、有放開）。
    std::function<std::unique_ptr<PTTDriver>()> debug_ptt_driver;

    /// 從檔案讀。path 是空字串時只存在記憶體裡（示範與測試用）。讀完會把不合理的值改回預設。
    static Settings load(const std::string& path);
    /// 和上次存的不一樣才寫檔。
    void save();

    /// 底列與設定視窗顯示的 PTT 說明
    std::string ptt_summary() const;
    /// 選的電台型號的名稱；還沒選的時候是空的
    std::string rig_model_title() const;
    /// 畫布目前的內容；沒有打開（或沒有字）的時候是空的
    std::optional<CanvasStyle> canvas_style() const;
    std::unique_ptr<PTTDriver> make_ptt_driver() const;
    float output_gain() const;

private:
    std::string serialized() const;
    std::string path_;
    std::string saved_;
};

/// 定時發射的時刻：照時鐘、對齊整點的固定間隔（每 15 分鐘就是每小時的 00、15、30、45 分整）。時間都是「1970 年起的秒數」。
namespace schedule {
/// 可以選的間隔（分鐘）。都除得盡 60，所以每個小時的時刻都一樣。
const std::vector<int>& intervals();
inline constexpr int kDefaultInterval = 15;
/// time 之後最近的一個時刻（不含 time 本身）：分是 interval_minutes 的倍數、秒是 0。照本地時間的整點算。
double next_fire(double time, int interval_minutes);
/// 一小時裡是哪幾分：「00、15、30、45」。太多個時只列頭三個與最後一個：「00、05、10 … 55」。
std::string minutes_text(int interval_minutes);
/// 「12:15」：本地時間，24 小時制。
std::string clock_text(double time);
/// 「14:32:07」
std::string time_text(double time);
/// 存檔的檔名裡用的「20261125-143207」
std::string stamp_text(double time);
/// 現在的時間
double now();
}  // namespace schedule

}  // namespace nbtvgui
