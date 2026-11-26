// 發射來源：鏡頭（SDL3 的相機 API）、檔案（影片或圖片）與圖片序列，以及把來源的一格縮成送出的大小
// （96×72；模式 5–7 與 15 是 192×144）。
// 對應 macOS 版的 VideoSources.swift 與 ImageSequence.swift。
//
// 影片檔靠使用者電腦上的 ffmpeg 解碼（這個程式自己不帶影片的解碼器）：播放時開一個 ffmpeg 把畫面從管線送過來，
// 這裡照實際的速度一格一格換上去；暫停就把它結束、記住位置，再播放或移動位置時從那個位置重新開一個。
// 圖片（JPEG、PNG、BMP、GIF）自己讀，不需要 ffmpeg；JPEG 照檔案裡記載的方向（EXIF）轉正。
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core_bridge.hpp"
#include "process.hpp"

namespace nbtvgui {

enum class SourceKind {
    camera,
    video,
    images,   // 圖片序列：選好的一批圖片，一張一張送
};
enum class ScaleMode {
    fill,   // 填滿：等比例放大到蓋滿 4:3，裁掉多出來的部分
    fit,    // 完整：整張都留著，空出來的地方補黑
};

/// 來源的一格畫面：RGB，每個像素三個位元組。
struct SourceFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
    /// 來源原本的大小。影片解碼時先縮小過（最後只要 96×72 或 192×144，不必整張解出來），畫面上顯示的「來源 1920×1080」要的是原本的；
    /// 0 表示就是 width、height。
    int native_width = 0;
    int native_height = 0;
    int shown_width() const { return native_width > 0 ? native_width : width; }
    int shown_height() const { return native_height > 0 ? native_height : height; }
};
using SourceFramePtr = std::shared_ptr<const SourceFrame>;

/// 把任意大小的畫面縮成 size 這麼大（預設是 96×72）。縮小時每個輸出的像素是它蓋到的那一塊來源像素的平均。
Picture scale_frame(const SourceFrame& frame, ScaleMode mode, PictureSize size = kStandardPicture);

/// 一張已經縮好的畫面換成另一種大小（模式換成畫面大小不同的那一種時，手上只有上一格縮好的畫面）。
/// 已經是那個大小就原樣回傳。
Picture resize_picture(const Picture& picture, PictureSize size);

/// 同樣是換成另一種大小，但不內插也不平均：每個輸出的像素直接取它對到的那個來源像素（最近鄰）。
/// 96×72 換成 192×144 就是每個像素重複成 2×2。內附的測試圖、存成影片時一段裡大小不同的畫面是這樣放大的。
Picture replicate_picture(const Picture& picture, PictureSize size);

/// 讀一張圖片檔（JPEG、PNG、BMP、GIF…）。讀不了回傳空的。
/// 手機直拍的相片存的是橫的像素加上「要轉 90 度」的記號（EXIF 的方向）：JPEG 照那個記號轉正。
/// 有很多格的檔案（GIF 動畫）只取第一格。
SourceFramePtr load_image_file(const std::string& path);

/// JPEG 檔的位元組裡記載的顯示方向（EXIF 的 1–8）；沒有記載、看不懂，或不是 JPEG 時是 1（不必轉）。
int jpeg_orientation(const std::uint8_t* bytes, std::size_t size);
/// 照記載的方向把畫面轉正（orientation 是 EXIF 的 1–8；5–8 轉了 90 度，寬高會對調）。
std::shared_ptr<SourceFrame> oriented(std::shared_ptr<SourceFrame> frame, int orientation);

/// 發射來源「圖片序列」：使用者選的一批圖片檔，照檔名排好，發射時一張一張送出去。
/// 這裡只管檔案：哪些是圖片、怎麼排、把一張圖讀進來（照記載的方向轉正、縮到夠用的大小）。
/// 送到第幾張、什麼時候換下一張由 App 決定（見 app.cpp「發射來源：圖片序列」那一段）。對應 macOS 版的 ImageSequence.swift。
class ImageSequence {
public:
    /// 讀得進來的圖片的副檔名（小寫，不含點）：jpg、jpeg、png、bmp、gif…
    static const std::vector<std::string>& extensions();
    /// 這個檔案是不是讀得進來的圖片。只看副檔名，不開檔。
    static bool is_image(const std::string& path);
    /// 檔名的順序：數字照大小排（2.jpg 在 10.jpg 前面），不分大小寫。a 排在 b 前面時回傳 true。
    static bool natural_less(const std::string& a, const std::string& b);
    /// 使用者選的東西 → 要送的圖片檔，照發射的順序排好。
    /// 選到的資料夾換成它裡面的圖片（只看那一層，不往子資料夾裡找；隱藏的檔案不算）；不是圖片的檔案略過；同一個檔案只算一次。
    /// 在開檔的對話框裡先點哪一張、後點哪一張不影響順序（對話框本來就不保證照點的順序給）。
    static std::vector<std::string> files(const std::vector<std::string>& selection);

    /// 讀進來的圖，短邊至少留這麼多像素（384）：96×72 的畫面長邊的四倍，最後縮成送出的大小時品質不受影響。
    /// 畫面是 192×144 的模式（5–7 與 15）也夠用：短邊是畫面高度的 2.7 倍，還是往小的縮。
    static constexpr int kShortSidePixels = 4 * (kImageWidth > kImageHeight ? kImageWidth : kImageHeight);
    /// 把一張圖讀進來：照記載的方向轉正、縮到夠用的大小（幾千萬像素的相片不必整張留著）。
    /// 讀不到（檔案不見了、壞了、其實不是圖片）回傳空的。原圖轉正之後的大小在 shown_width()、shown_height()。
    static SourceFramePtr load(const std::string& path);

    /// 同 load，但最近讀過的幾張（預覽的那一張、它的下一張，再多留一張）直接給。
    /// 只在一條執行緒上使用（App 的 preview_worker_），所以不必上鎖。
    SourceFramePtr picture(const std::string& path);
    /// 換了一批圖片：之前讀進來的都丟掉（同一個檔名可能已經是不同的內容）。
    void forget() { recent_.clear(); }

private:
    static constexpr std::size_t kKeep = 3;
    std::vector<std::pair<std::string, SourceFramePtr>> recent_;   // 最新的在最後
};

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
