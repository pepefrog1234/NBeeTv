#include "sources.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "i18n.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace nbtvgui {

// ---------------------------------------------------------------- 縮放

Picture scale_frame(const SourceFrame& frame, ScaleMode mode) {
    Picture out;   // 全黑
    const int sw = frame.width, sh = frame.height;
    if (sw <= 0 || sh <= 0 || frame.rgb.size() < static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 3) return out;
    const double rx = static_cast<double>(kImageWidth) / sw, ry = static_cast<double>(kImageHeight) / sh;
    const double ratio = mode == ScaleMode::fill ? std::max(rx, ry) : std::min(rx, ry);
    // 輸出的像素 (ox, oy) 蓋到來源的哪一塊：以兩邊的中心對齊
    const auto source_x = [&](double ox) { return (ox - kImageWidth / 2.0) / ratio + sw / 2.0; };
    const auto source_y = [&](double oy) { return (oy - kImageHeight / 2.0) / ratio + sh / 2.0; };
    for (int oy = 0; oy < kImageHeight; ++oy) {
        const double y0 = source_y(oy), y1 = source_y(oy + 1);
        for (int ox = 0; ox < kImageWidth; ++ox) {
            const double x0 = source_x(ox), x1 = source_x(ox + 1);
            double sum[3] = {0, 0, 0}, covered = 0;
            if (ratio < 1.0) {
                // 縮小：這一塊裡每個來源像素照蓋到的面積加權平均；落在來源外面的部分是黑的
                const int ya = std::max(0, static_cast<int>(std::floor(y0))), yb = std::min(sh, static_cast<int>(std::ceil(y1)));
                const int xa = std::max(0, static_cast<int>(std::floor(x0))), xb = std::min(sw, static_cast<int>(std::ceil(x1)));
                for (int y = ya; y < yb; ++y) {
                    const double wy = std::min<double>(y + 1, y1) - std::max<double>(y, y0);
                    if (wy <= 0) continue;
                    const std::uint8_t* row = frame.rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) * 3;
                    for (int x = xa; x < xb; ++x) {
                        const double w = wy * (std::min<double>(x + 1, x1) - std::max<double>(x, x0));
                        if (w <= 0) continue;
                        sum[0] += w * row[3 * x];
                        sum[1] += w * row[3 * x + 1];
                        sum[2] += w * row[3 * x + 2];
                    }
                }
                covered = (y1 - y0) * (x1 - x0);
            } else {
                // 放大：取中心點，在最近的四個來源像素之間內插
                const double cx = (x0 + x1) / 2 - 0.5, cy = (y0 + y1) / 2 - 0.5;
                if (cx > -1 && cy > -1 && cx < sw && cy < sh) {
                    const int xa = static_cast<int>(std::floor(cx)), ya = static_cast<int>(std::floor(cy));
                    const double fx = cx - xa, fy = cy - ya;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int x = xa + dx, y = ya + dy;
                            if (x < 0 || y < 0 || x >= sw || y >= sh) continue;
                            const double w = (dx == 0 ? 1 - fx : fx) * (dy == 0 ? 1 - fy : fy);
                            const std::uint8_t* p = frame.rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(x)) * 3;
                            sum[0] += w * p[0];
                            sum[1] += w * p[1];
                            sum[2] += w * p[2];
                        }
                    }
                }
                covered = 1;
            }
            std::uint8_t* dst = out.rgb.data() + (static_cast<std::size_t>(oy) * kImageWidth + static_cast<std::size_t>(ox)) * 3;
            for (int c = 0; c < 3; ++c) dst[c] = static_cast<std::uint8_t>(std::clamp(covered > 0 ? sum[c] / covered + 0.5 : 0.0, 0.0, 255.0));
        }
    }
    return out;
}

SourceFramePtr load_image_file(const std::string& path) {
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 3);
    if (pixels == nullptr) return nullptr;
    auto frame = std::make_shared<SourceFrame>();
    frame->width = width;
    frame->height = height;
    frame->rgb.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    stbi_image_free(pixels);
    return frame;
}

// ---------------------------------------------------------------- 鏡頭

std::vector<CameraInfo> CameraSource::devices() {
    std::vector<CameraInfo> cameras;
    int count = 0;
    SDL_CameraID* ids = SDL_GetCameras(&count);
    if (ids == nullptr) return cameras;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetCameraName(ids[i]);
        cameras.push_back({name != nullptr && *name != '\0' ? name : "Camera " + std::to_string(i + 1), ids[i]});
    }
    SDL_free(ids);
    return cameras;
}

std::optional<std::string> CameraSource::start(const std::string& name) {
    stop();
    const std::vector<CameraInfo> cameras = devices();
    if (cameras.empty()) return L("找不到鏡頭");
    SDL_CameraID id = cameras.front().id;
    for (const CameraInfo& camera : cameras) {
        if (camera.name == name) id = camera.id;
    }
    // 畫面最後要縮成 96×72，不需要鏡頭最大的解析度：挑一個夠用（至少 384×288）而最小的格式，轉換起來省事
    SDL_CameraSpec chosen{};
    bool have_spec = false;
    int count = 0;
    if (SDL_CameraSpec** specs = SDL_GetCameraSupportedFormats(id, &count)) {
        long best = 0;
        for (int i = 0; i < count; ++i) {
            const SDL_CameraSpec& spec = *specs[i];
            if (spec.width < 384 || spec.height < 288) continue;
            const long area = static_cast<long>(spec.width) * spec.height;
            if (!have_spec || area < best) {
                chosen = spec;
                best = area;
                have_spec = true;
            }
        }
        SDL_free(specs);
    }
    camera_ = SDL_OpenCamera(id, have_spec ? &chosen : nullptr);
    if (camera_ == nullptr) return LF("無法開啟鏡頭：%@", {SDL_GetError()});
    return std::nullopt;
}

void CameraSource::stop() {
    if (camera_ == nullptr) return;
    SDL_CloseCamera(camera_);
    camera_ = nullptr;
}

bool CameraSource::denied() const { return camera_ != nullptr && SDL_GetCameraPermissionState(camera_) < 0; }

SourceFramePtr CameraSource::poll() {
    if (camera_ == nullptr || SDL_GetCameraPermissionState(camera_) <= 0) return nullptr;
    // 把排著的都取出來，只留最新的一格
    SDL_Surface* latest = nullptr;
    for (;;) {
        Uint64 timestamp = 0;
        SDL_Surface* next = SDL_AcquireCameraFrame(camera_, &timestamp);
        if (next == nullptr) break;
        if (latest != nullptr) SDL_ReleaseCameraFrame(camera_, latest);
        latest = next;
    }
    if (latest == nullptr) return nullptr;
    SourceFramePtr result;
    if (SDL_Surface* rgb = SDL_ConvertSurface(latest, SDL_PIXELFORMAT_RGB24)) {
        auto frame = std::make_shared<SourceFrame>();
        frame->width = rgb->w;
        frame->height = rgb->h;
        frame->rgb.resize(static_cast<std::size_t>(rgb->w) * static_cast<std::size_t>(rgb->h) * 3);
        for (int y = 0; y < rgb->h; ++y) {
            std::memcpy(frame->rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rgb->w) * 3,
                        static_cast<const std::uint8_t*>(rgb->pixels) + static_cast<std::size_t>(y) * static_cast<std::size_t>(rgb->pitch), static_cast<std::size_t>(rgb->w) * 3);
        }
        SDL_DestroySurface(rgb);
        result = frame;
    }
    SDL_ReleaseCameraFrame(camera_, latest);
    return result;
}

// ---------------------------------------------------------------- 檔案

namespace {

constexpr double kDecodeFps = 10.0;   // 向 ffmpeg 要的畫面更新率：最快的模式每 0.67 秒才送一張，夠了

std::string find_ffmpeg_tool(const char* name) {
    return find_program(name, {
#if !defined(_WIN32)
                                  "/usr/bin", "/usr/local/bin", "/opt/homebrew/bin", "/opt/local/bin"
#endif
                              });
}

struct VideoInfo {
    double duration = 0;   // 秒；問不出來是 0
    int width = 0;         // 畫面原本的大小；問不出來是 0
    int height = 0;
};

/// 影片多長、畫面多大。
VideoInfo probe_video(const std::string& path) {
    VideoInfo info;
    const std::string ffprobe = find_ffmpeg_tool("ffprobe");
    if (!ffprobe.empty()) {
        // 印出來的是「width=1920」「height=1080」「duration=38.000000」各一行
        if (const auto text = run_and_capture({ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height:format=duration", "-of", "default=noprint_wrappers=1", path}, 10)) {
            const auto number_after = [&text](const char* key) -> double {
                const std::size_t at = text->find(key);
                return at == std::string::npos ? 0.0 : std::atof(text->c_str() + at + std::strlen(key));
            };
            info.width = static_cast<int>(number_after("width="));
            info.height = static_cast<int>(number_after("height="));
            info.duration = number_after("duration=");
            if (info.duration > 0) return info;
        }
    }
    // 沒有 ffprobe：ffmpeg 沒有給輸出檔時會印出檔案的資訊再結束，裡面有「Duration: 00:00:38.04」與「Video: h264 …, 1920x1080 …」
    const std::string ffmpeg = FileSource::ffmpeg_path();
    if (ffmpeg.empty()) return info;
    const auto text = run_and_capture({ffmpeg, "-hide_banner", "-nostdin", "-i", path}, 10, true);
    if (!text) return info;
    if (const std::size_t at = text->find("Duration: "); at != std::string::npos) {
        int hours = 0, minutes = 0;
        double seconds = 0;
        if (std::sscanf(text->c_str() + at + 10, "%d:%d:%lf", &hours, &minutes, &seconds) == 3) info.duration = hours * 3600.0 + minutes * 60.0 + seconds;
    }
    if (std::size_t at = text->find("Video: "); at != std::string::npos && info.width == 0) {
        // 「Video:」那一行裡第一個「, 數字x數字」
        const std::size_t end = text->find('\n', at);
        while ((at = text->find(", ", at)) != std::string::npos && (end == std::string::npos || at < end)) {
            at += 2;
            int width = 0, height = 0;
            if (std::sscanf(text->c_str() + at, "%dx%d", &width, &height) == 2 && width >= 16 && height >= 16) {
                info.width = width;
                info.height = height;
                break;
            }
        }
    }
    return info;
}

/// 從 ffmpeg 送過來的一串 PPM（P6）裡讀下一張。讀到結尾回傳空的。
std::shared_ptr<SourceFrame> read_ppm(SDL_IOStream* stream) {
    // 檔頭：「P6\n寬 高\n255\n」
    char magic[2];
    if (!read_exact(stream, magic, 2) || magic[0] != 'P' || magic[1] != '6') return nullptr;
    int numbers[3] = {0, 0, 0};
    for (int& number : numbers) {
        char c = 0;
        do {
            if (!read_exact(stream, &c, 1)) return nullptr;
        } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        while (c >= '0' && c <= '9') {
            number = number * 10 + (c - '0');
            if (!read_exact(stream, &c, 1)) return nullptr;
        }
    }
    if (numbers[0] <= 0 || numbers[1] <= 0 || numbers[0] > 8192 || numbers[1] > 8192 || numbers[2] != 255) return nullptr;
    auto frame = std::make_shared<SourceFrame>();
    frame->width = numbers[0];
    frame->height = numbers[1];
    frame->rgb.resize(static_cast<std::size_t>(numbers[0]) * static_cast<std::size_t>(numbers[1]) * 3);
    if (!read_exact(stream, frame->rgb.data(), frame->rgb.size())) return nullptr;
    return frame;
}

}  // namespace

FileSource::FileSource() = default;

FileSource::~FileSource() { stop_decoder(); }

std::string FileSource::ffmpeg_path() {
    static std::mutex mutex;
    static std::string cached;
    std::lock_guard<std::mutex> lock(mutex);
    if (cached.empty()) cached = find_ffmpeg_tool("ffmpeg");   // 還沒找到的話每次都再找：使用者可能剛裝好
    return cached;
}

bool FileSource::ffmpeg_available() { return !ffmpeg_path().empty(); }

std::optional<std::string> FileSource::open(const std::string& path) {
    close();
    if (SourceFramePtr image = load_image_file(path)) {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        image_ = true;
        duration_ = 0;
        position_ = 0;
        frame_ = image;
        ++version_;
        return std::nullopt;
    }
    if (!ffmpeg_available()) return L("要用影片檔，這台電腦要先裝 ffmpeg（圖片檔不需要）");
    const VideoInfo info = probe_video(path);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        image_ = false;
        duration_ = info.duration;
        native_width_ = info.width;
        native_height_ = info.height;
        position_ = 0;
        playing_ = false;
        ended_ = false;
        frame_.reset();
    }
    // 取開頭的一格：取得到才算開得起來
    start_decoder(0, true);
    if (reader_.joinable()) reader_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_) {
        path_.clear();
        duration_ = 0;
        return L("無法播放這個影片檔");
    }
    return std::nullopt;
}

void FileSource::close() {
    stop_decoder();
    std::lock_guard<std::mutex> lock(mutex_);
    path_.clear();
    image_ = false;
    duration_ = 0;
    native_width_ = 0;
    native_height_ = 0;
    position_ = 0;
    playing_ = false;
    ended_ = false;
    frame_.reset();
    ++version_;
}

bool FileSource::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !path_.empty();
}

bool FileSource::is_image() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return image_;
}

std::string FileSource::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

double FileSource::duration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return duration_;
}

void FileSource::set_loop(bool loop) {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = loop;
}

bool FileSource::is_playing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
}

bool FileSource::has_ended() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_;
}

double FileSource::position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return position_;
}

SourceFramePtr FileSource::current_frame(std::uint64_t* version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (version != nullptr) *version = version_;
    return frame_;
}

void FileSource::play() {
    double from = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_.empty() || image_ || playing_) return;
        from = ended_ ? 0 : position_;   // 停在結尾的從頭來
        ended_ = false;
        playing_ = true;
    }
    start_decoder(from, false);
}

void FileSource::pause() {
    stop_decoder();
    std::lock_guard<std::mutex> lock(mutex_);
    playing_ = false;
}

void FileSource::seek(double seconds) {
    bool playing = false;
    double target = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_.empty() || image_) return;
        target = std::clamp(seconds, 0.0, duration_ > 0 ? duration_ : seconds);
        position_ = target;
        ended_ = false;
        playing = playing_;
    }
    start_decoder(target, !playing);   // 暫停著的時候只取那個位置的一格當預覽
}

void FileSource::start_decoder(double from, bool single_frame) {
    stop_decoder();
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation = ++generation_;
    }
    reader_ = std::thread([this, generation, from, single_frame] { decode_loop(generation, from, single_frame); });
}

void FileSource::stop_decoder() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        if (decoder_) decoder_->kill(true);   // 管線跟著斷，讀的那條執行緒就會結束
    }
    if (reader_.joinable()) reader_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    decoder_.reset();
}

void FileSource::decode_loop(std::uint64_t generation, double from, bool single_frame) {
    for (;;) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;
            path = path_;
        }
        char position[32];
        std::snprintf(position, sizeof position, "%.3f", from);
        Process::Options options;
        options.args = {ffmpeg_path(), "-nostdin", "-hide_banner", "-loglevel", "error", "-ss", position, "-i", path, "-an", "-sn", "-dn", "-vf"};
        char filter[96];
        std::snprintf(filter, sizeof filter, "fps=%.0f,scale=512:512:force_original_aspect_ratio=decrease", kDecodeFps);
        options.args.push_back(filter);
        if (single_frame) options.args.insert(options.args.end(), {"-frames:v", "1"});
        options.args.insert(options.args.end(), {"-f", "image2pipe", "-c:v", "ppm", "-"});
        options.pipe_stdout = true;
        std::unique_ptr<Process> process = Process::spawn(options, nullptr);
        if (!process) return;
        SDL_IOStream* stream = process->out();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;   // 啟動的這段時間裡已經被叫停了
            decoder_ = std::move(process);
        }
        // 播放的速度由這裡控制，不靠 ffmpeg 的 -re：它在有些版本上開頭會先停兩三秒才開始送（Ubuntu 22.04 的 4.4 就是）。
        // ffmpeg 解得比播放快，解好的畫面卡在管線裡等我們讀（管線滿了它自己會停），我們照時間一格一格拿：
        // 第 k 格在開始後 k÷10 秒換上去。
        const auto began = std::chrono::steady_clock::now();
        long frames = 0;
        while (std::shared_ptr<SourceFrame> frame = read_ppm(stream)) {
            if (!single_frame) {
                const auto due = began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(static_cast<double>(frames) / kDecodeFps));
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (generation != generation_) return;   // 暫停、移動位置或關檔了
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= due) break;
                    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(due - now, std::chrono::milliseconds(20)));
                }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;
            // 原本的大小：解出來的這一格是縮小過的。手機直拍的影片，檔案裡記的是橫的大小加上「要轉 90 度」，
            // ffmpeg 解出來已經轉好了——方向和記的不一樣時寬高對調
            if (native_width_ > 0 && native_height_ > 0) {
                const bool turned = (native_width_ > native_height_) != (frame->width > frame->height) && frame->width != frame->height;
                frame->native_width = turned ? native_height_ : native_width_;
                frame->native_height = turned ? native_width_ : native_height_;
            }
            frame_ = frame;
            ++version_;
            if (!single_frame) position_ = from + static_cast<double>(frames) / kDecodeFps;
            ++frames;
        }
        // 讀到結尾：影片播完了（或被叫停、ffmpeg 出錯）
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || single_frame) return;
        if (loop_ && frames > 0) {
            from = 0;   // 循環播放：從頭再來
            continue;
        }
        playing_ = false;
        ended_ = true;
        if (duration_ > 0) position_ = duration_;
        return;
    }
}

}  // namespace nbtvgui
