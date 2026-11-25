// 發射來源：鏡頭（SDL3 的相機 API）與檔案（影片或圖片），以及把來源的一格縮成 96×72。對應 macOS 版的 VideoSources.swift。
//
// 影片檔靠使用者電腦上的 ffmpeg 解碼（這個程式自己不帶影片的解碼器）：播放時開一個 ffmpeg 把畫面從管線送過來，
// 這裡照實際的速度一格一格換上去；暫停就把它結束、記住位置，再播放或移動位置時從那個位置重新開一個。
// 圖片（JPEG、PNG、BMP、GIF）自己讀，不需要 ffmpeg。
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core_bridge.hpp"
#include "process.hpp"

namespace nbtvgui {

enum class SourceKind { camera, video };
enum class ScaleMode {
    fill,   // 填滿：等比例放大到蓋滿 4:3，裁掉多出來的部分
    fit,    // 完整：整張都留著，空出來的地方補黑
};

/// 來源的一格畫面：RGB，每個像素三個位元組。
struct SourceFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
    /// 來源原本的大小。影片解碼時先縮小過（最後只要 96×72，不必整張解出來），畫面上顯示的「來源 1920×1080」要的是原本的；
    /// 0 表示就是 width、height。
    int native_width = 0;
    int native_height = 0;
    int shown_width() const { return native_width > 0 ? native_width : width; }
    int shown_height() const { return native_height > 0 ? native_height : height; }
};
using SourceFramePtr = std::shared_ptr<const SourceFrame>;

/// 把任意大小的畫面縮成 96×72。縮小時每個輸出的像素是它蓋到的那一塊來源像素的平均。
Picture scale_frame(const SourceFrame& frame, ScaleMode mode);

/// 讀一張圖片檔（JPEG、PNG、BMP、GIF…）。讀不了回傳空的。
SourceFramePtr load_image_file(const std::string& path);

struct CameraInfo {
    std::string name;
    SDL_CameraID id = 0;
};

class CameraSource {
public:
    ~CameraSource() { stop(); }
    /// 系統上的鏡頭。要先 SDL_InitSubSystem(SDL_INIT_CAMERA)。
    static std::vector<CameraInfo> devices();

    /// 開始擷取。name 是空字串（或那個鏡頭不在了）時用第一個。失敗時回傳原因。
    std::optional<std::string> start(const std::string& name);
    void stop();
    bool running() const { return camera_ != nullptr; }
    /// 系統拒絕了使用鏡頭的權限
    bool denied() const;
    /// 取最新的一格；沒有新的回傳空的。在主執行緒上定期呼叫。
    SourceFramePtr poll();

private:
    SDL_Camera* camera_ = nullptr;
};

class FileSource {
public:
    FileSource();
    ~FileSource();
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    /// 這台電腦上找得到 ffmpeg 嗎（影片要靠它）
    static bool ffmpeg_available();
    static std::string ffmpeg_path();

    /// 開啟一個檔案。圖片直接讀；其他的當成影片，問 ffmpeg 它多長、取開頭的一格。會花一點時間，不要在主執行緒上呼叫。
    /// 失敗時回傳原因。
    std::optional<std::string> open(const std::string& path);
    void close();

    bool is_open() const;
    bool is_image() const;
    std::string path() const;
    double duration() const;          // 圖片是 0
    /// 沒有勾循環播放時播到結尾就停在最後一格
    void set_loop(bool loop);

    void play();
    void pause();
    void seek(double seconds);
    /// 回到開頭（不改變播放或暫停）
    void rewind() { seek(0); }
    bool is_playing() const;
    /// 只播一次的影片已經播到結尾
    bool has_ended() const;
    double position() const;

    /// 目前這一格（還沒有任何一格時是空的），以及它是第幾格（每換一格加一）。
    SourceFramePtr current_frame(std::uint64_t* version = nullptr) const;

private:
    void start_decoder(double from, bool single_frame);
    void stop_decoder();
    void decode_loop(std::uint64_t generation, double from, bool single_frame);

    mutable std::mutex mutex_;
    std::string path_;
    bool image_ = false;
    double duration_ = 0;
    int native_width_ = 0;    // 影片原本的大小（問不出來是 0）
    int native_height_ = 0;
    bool loop_ = true;
    bool playing_ = false;
    bool ended_ = false;
    double position_ = 0;
    SourceFramePtr frame_;
    std::uint64_t version_ = 0;
    std::uint64_t generation_ = 0;       // 每開一次解碼的行程加一；舊的那一條執行緒看到變了就不再動狀態
    std::unique_ptr<Process> decoder_;
    std::thread reader_;
};

}  // namespace nbtvgui
