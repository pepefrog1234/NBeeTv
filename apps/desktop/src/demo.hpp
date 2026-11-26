// 示範圖（--snapshot、--demo）與自我測試（--selftest）共用的工具：內附的測試圖、模擬電台收到的音訊、示範影片。
// 影像與瀑布圖都是核心真的算出來的，這裡只負責準備輸入。對應 macOS 版的 Demo.swift。
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app.hpp"
#include "core_bridge.hpp"

namespace nbtvgui::demo {

/// 內附的測試圖（96×72）：coffee、rocket、cat（core/tests/data 裡的那三張，建置時編進執行檔）。
/// size 比 96×72 大時每個像素原樣重複放大（模式 5–7 與 15 的 192×144 是兩倍）；核心的測試（core/tests/test_images.hpp）
/// 也是這樣放大測試圖的。
std::optional<Picture> test_image(const std::string& name, PictureSize size = kStandardPicture);

/// 可重現的亂數（示範圖每次算出來都一樣）。
struct Random {
    explicit Random(std::uint64_t seed) : state(seed) {}
    std::uint64_t next();
    double uniform();
    double gauss();
    std::uint64_t state;
};

double power(const std::vector<float>& samples);
std::vector<float> silence(double seconds);

/// 模擬 SSB 電台收進來的音訊：加白雜訊、整個頻譜平移（電台沒對準頻率）、過電台的通帶（約 250–2750 Hz）。
///   signal_power：訊號在發射期間的平均功率，用來換算雜訊；snr_db 是 3 kHz 雜訊頻寬的訊雜比，沒有給就不加雜訊。
///   noise_only_level_db：沒有訊號時（signal_power 是 0）雜訊過通帶後的音量。
///   output_level_db：有給的話，把訊號期間的音量調到這個值（dBFS）。
std::vector<float> radio_channel(const std::vector<float>& input, double signal_power, std::optional<double> snr_db, double shift_hz, double noise_only_level_db = -32,
                                 std::optional<double> output_level_db = std::nullopt, std::uint64_t seed = 2026);

/// 把一段音訊分塊餵給接收機與頻譜分析（像音訊裝置那樣一次一小段）。回傳每一塊的更新，順序與實際接收時相同。
std::vector<RxUpdate> receive(const std::vector<float>& audio);

/// 把一段送出去的音訊交給頻譜分析（發射監看），回傳頻譜列。
std::vector<std::array<float, kSpectrumBins>> monitor_rows(const std::vector<float>& audio);

/// 把一張畫面每個像素放大成 zoom×zoom，存成 PNG（96×72 的畫面放大十倍是 960×720）。
bool write_picture_png(const Picture& picture, const std::string& path, int zoom = 10);

/// 同上，存成 JPEG。orientation 不是 0 的話，在檔案裡記下顯示的方向（EXIF 的 1–8；手機直拍的相片是 6：
/// 存的像素是橫的，看的時候要順時針轉 90 度）——像素本身不動。
/// little_endian、after_jfif：EXIF 那一段的兩種寫法（位元組的順序）與兩種位置（最前面，或排在 JFIF 那一段後面）；
/// 各家相機與軟體寫的不一樣，讀的時候都要認得。
bool write_picture_jpeg(const Picture& picture, const std::string& path, int zoom = 10, int orientation = 0, bool little_endian = false, bool after_jfif = false);

/// 用一張圖做一段示範影片（靠 ffmpeg）：畫面慢慢往上移，每一格都不一樣。沒有 ffmpeg 或做不出來回傳 false。
bool make_video(const std::string& path, const Picture& picture, int seconds = 38, int fps = 5, int width = 1920, int height = 1080);

/// 一邊做主執行緒排著的工作（還有 app 的 tick，有給的話）、一邊等條件成立，最多等 timeout 秒。
bool wait_until(App* app, double timeout, const std::function<bool()>& condition);
/// 讓主執行緒排著的工作跑這麼久。
void spin(App* app, double seconds);

/// 兩張一樣大的畫面相差多少（PSNR，dB）；完全一樣是 99。大小不一樣的當成完全不像，是 0。
double psnr(const Picture& a, const Picture& b);

}  // namespace nbtvgui::demo
