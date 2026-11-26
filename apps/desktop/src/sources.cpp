#include "sources.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

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

Picture scale_frame(const SourceFrame& frame, ScaleMode mode, PictureSize size) {
    Picture out(size);   // 全黑
    const int sw = frame.width, sh = frame.height;
    const int width = size.width, height = size.height;
    if (width <= 0 || height <= 0) return out;
    if (sw <= 0 || sh <= 0 || frame.rgb.size() < static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 3) return out;
    const double rx = static_cast<double>(width) / sw, ry = static_cast<double>(height) / sh;
    const double ratio = mode == ScaleMode::fill ? std::max(rx, ry) : std::min(rx, ry);
    // 輸出的像素 (ox, oy) 蓋到來源的哪一塊：以兩邊的中心對齊
    const auto source_x = [&](double ox) { return (ox - width / 2.0) / ratio + sw / 2.0; };
    const auto source_y = [&](double oy) { return (oy - height / 2.0) / ratio + sh / 2.0; };
    for (int oy = 0; oy < height; ++oy) {
        const double y0 = source_y(oy), y1 = source_y(oy + 1);
        for (int ox = 0; ox < width; ++ox) {
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
            std::uint8_t* dst = out.rgb.data() + (static_cast<std::size_t>(oy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(ox)) * 3;
            for (int c = 0; c < 3; ++c) dst[c] = static_cast<std::uint8_t>(std::clamp(covered > 0 ? sum[c] / covered + 0.5 : 0.0, 0.0, 255.0));
        }
    }
    return out;
}

Picture resize_picture(const Picture& picture, PictureSize size) {
    if (picture.size == size) return picture;
    SourceFrame frame;
    frame.width = picture.width();
    frame.height = picture.height();
    frame.rgb = picture.rgb;
    return scale_frame(frame, ScaleMode::fill, size);
}

Picture replicate_picture(const Picture& picture, PictureSize size) {
    if (picture.size == size) return picture;
    Picture out(size);
    const int sw = picture.width(), sh = picture.height();
    if (sw <= 0 || sh <= 0 || size.width <= 0 || size.height <= 0 || picture.rgb.size() < static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 3) return out;
    for (int y = 0; y < size.height; ++y) {
        const std::uint8_t* row = picture.rgb.data() + static_cast<std::size_t>(std::min(sh - 1, y * sh / size.height)) * static_cast<std::size_t>(sw) * 3;
        std::uint8_t* dst = out.rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width) * 3;
        for (int x = 0; x < size.width; ++x) std::memcpy(dst + static_cast<std::size_t>(x) * 3, row + static_cast<std::size_t>(std::min(sw - 1, x * sw / size.width)) * 3, 3);
    }
    return out;
}

// ---------------------------------------------------------------- 圖片檔

namespace {

/// EXIF 的內容（TIFF 的格式）裡「方向」那一項（0x0112）；沒有或看不懂是 1。
int tiff_orientation(const std::uint8_t* t, std::size_t size) {
    if (size < 8) return 1;
    const bool little = t[0] == 'I' && t[1] == 'I', big = t[0] == 'M' && t[1] == 'M';
    if (!little && !big) return 1;
    const auto u16 = [&](std::size_t at) { return little ? static_cast<std::uint32_t>(t[at] | (t[at + 1] << 8)) : static_cast<std::uint32_t>((t[at] << 8) | t[at + 1]); };
    const auto u32 = [&](std::size_t at) { return little ? u16(at) | (u16(at + 2) << 16) : (u16(at) << 16) | u16(at + 2); };
    if (u16(2) != 42) return 1;
    const std::size_t directory = u32(4);
    if (directory > size - 2) return 1;
    const std::size_t count = u16(directory);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t entry = directory + 2 + 12 * i;
        if (entry + 12 > size) break;
        if (u16(entry) != 0x0112) continue;
        // 規格上是一個 16 位元的整數；有的程式寫成 32 位元的
        const std::uint32_t type = u16(entry + 2);
        const std::uint32_t value = type == 3 ? u16(entry + 8) : type == 4 ? u32(entry + 8) : 0;
        return value >= 1 && value <= 8 ? static_cast<int>(value) : 1;
    }
    return 1;
}

}  // namespace

int jpeg_orientation(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) return 1;
    // JPEG 是一段一段的：0xFF、哪一種、兩個位元組的長度（含這兩個位元組自己）、內容。EXIF 在影像資料之前的 APP1 那一段
    std::size_t at = 2;
    while (at + 4 <= size) {
        if (bytes[at] != 0xFF) return 1;   // 不是一段的開頭：檔案不對勁，不猜
        const std::uint8_t marker = bytes[at + 1];
        if (marker == 0xFF) {   // 段與段之間的填充
            ++at;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {   // 沒有長度的那幾種
            at += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) return 1;   // 影像資料開始了（或檔案結束）：前面沒有 EXIF
        const std::size_t length = (static_cast<std::size_t>(bytes[at + 2]) << 8) | bytes[at + 3];
        if (length < 2) return 1;
        const std::size_t data = at + 4, end = at + 2 + length;
        if (marker == 0xE1 && length >= 2 + 6 + 8 && end <= size && std::memcmp(bytes + data, "Exif\0\0", 6) == 0) {
            return tiff_orientation(bytes + data + 6, end - data - 6);
        }
        at = end;
    }
    return 1;
}

std::shared_ptr<SourceFrame> oriented(std::shared_ptr<SourceFrame> frame, int orientation) {
    if (!frame || orientation <= 1 || orientation > 8) return frame;
    const int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0 || frame->rgb.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3) return frame;
    // 5–8 是轉了 90 度的：轉正之後寬高對調
    const bool sideways = orientation >= 5;
    auto upright = std::make_shared<SourceFrame>();
    upright->width = sideways ? h : w;
    upright->height = sideways ? w : h;
    upright->native_width = sideways ? frame->native_height : frame->native_width;
    upright->native_height = sideways ? frame->native_width : frame->native_height;
    upright->rgb.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    const std::uint8_t* source = frame->rgb.data();
    std::uint8_t* out = upright->rgb.data();
    for (int y = 0; y < upright->height; ++y) {
        for (int x = 0; x < upright->width; ++x) {
            // 轉正之後的 (x, y) 是存檔裡的哪一個像素 (sx, sy)。EXIF 的定義：存檔的第 0 列、第 0 行各在畫面的哪一邊
            int sx = x, sy = y;
            switch (orientation) {
            case 2: sx = w - 1 - x; break;                   // 左右相反
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;   // 轉了 180 度
            case 4: sy = h - 1 - y; break;                   // 上下相反
            case 5: sx = y; sy = x; break;                   // 沿左上到右下的對角線翻
            case 6: sx = y; sy = h - 1 - x; break;           // 要順時針轉 90 度（手機直拍）
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;   // 沿右上到左下的對角線翻
            default: sx = w - 1 - y; sy = x; break;          // 8：要逆時針轉 90 度
            }
            std::memcpy(out, source + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx)) * 3, 3);
            out += 3;
        }
    }
    return upright;
}

SourceFramePtr load_image_file(const std::string& path) {
    // 記載的方向在檔案開頭附近（JPEG 的 EXIF 那一段，在影像資料之前）：先讀開頭一小段看有沒有。
    // 不整個檔案讀進來：這個函式也用來試「這個檔案是不是圖片」，對方可能是幾 GB 的影片
    int orientation = 1;
    if (SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "rb")) {
        std::vector<std::uint8_t> head(256 * 1024);
        const std::size_t got = SDL_ReadIO(file, head.data(), head.size());
        SDL_CloseIO(file);
        orientation = jpeg_orientation(head.data(), got);
    }
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 3);
    if (pixels == nullptr) return nullptr;
    auto frame = std::make_shared<SourceFrame>();
    frame->width = width;
    frame->height = height;
    frame->rgb.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    stbi_image_free(pixels);
    return oriented(std::move(frame), orientation);
}

// ---------------------------------------------------------------- 圖片序列

namespace {

/// 把畫面縮小成 width×height（比原來小）：每個輸出的像素是它蓋到的那一塊來源像素的平均。
std::shared_ptr<SourceFrame> shrink_frame(const SourceFrame& source, int width, int height) {
    const int sw = source.width, sh = source.height;
    auto out = std::make_shared<SourceFrame>();
    out->width = width;
    out->height = height;
    out->rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    const double step_x = static_cast<double>(sw) / width, step_y = static_cast<double>(sh) / height;
    // 橫的方向先縮（一列來源變成一列 width 個點），直的方向再把蓋到的那幾列加權平均
    std::vector<float> row(static_cast<std::size_t>(width) * 3), sum(static_cast<std::size_t>(width) * 3);
    int row_of = -1;   // row 裡現在是來源的第幾列
    const auto reduce_row = [&](int sy) {
        const std::uint8_t* line = source.rgb.data() + static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) * 3;
        for (int ox = 0; ox < width; ++ox) {
            const double x0 = ox * step_x, x1 = (ox + 1) * step_x;
            const int from = std::max(0, static_cast<int>(std::floor(x0))), to = std::min(sw, static_cast<int>(std::ceil(x1)));
            float total[3] = {0, 0, 0};
            for (int sx = from; sx < to; ++sx) {
                const float weight = static_cast<float>(std::min<double>(sx + 1, x1) - std::max<double>(sx, x0));
                if (weight <= 0) continue;
                total[0] += weight * line[3 * sx];
                total[1] += weight * line[3 * sx + 1];
                total[2] += weight * line[3 * sx + 2];
            }
            const float width_covered = static_cast<float>(x1 - x0);
            for (int c = 0; c < 3; ++c) row[static_cast<std::size_t>(ox) * 3 + static_cast<std::size_t>(c)] = total[c] / width_covered;
        }
        row_of = sy;
    };
    for (int oy = 0; oy < height; ++oy) {
        const double y0 = oy * step_y, y1 = (oy + 1) * step_y;
        const int from = std::max(0, static_cast<int>(std::floor(y0))), to = std::min(sh, static_cast<int>(std::ceil(y1)));
        std::fill(sum.begin(), sum.end(), 0.0f);
        for (int sy = from; sy < to; ++sy) {
            const float weight = static_cast<float>(std::min<double>(sy + 1, y1) - std::max<double>(sy, y0));
            if (weight <= 0) continue;
            if (row_of != sy) reduce_row(sy);   // 上一列輸出的最後一列常常就是這一列輸出的第一列：不必再算一次
            for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += weight * row[i];
        }
        const float height_covered = static_cast<float>(y1 - y0);
        std::uint8_t* line = out->rgb.data() + static_cast<std::size_t>(oy) * static_cast<std::size_t>(width) * 3;
        for (std::size_t i = 0; i < sum.size(); ++i) line[i] = static_cast<std::uint8_t>(std::clamp(sum[i] / height_covered + 0.5f, 0.0f, 255.0f));
    }
    return out;
}

/// 路徑整理成固定的寫法（從根目錄寫起，去掉多餘的「.」「..」與重複的分隔字元），同一個檔案才認得出來，
/// 記進設定之後、下次從別的地方啟動也還找得到。
std::string normalized_path(const std::string& path) {
    const std::filesystem::path given(std::u8string(path.begin(), path.end()));   // 路徑一律是 UTF-8
    std::error_code error;
    std::filesystem::path full = std::filesystem::absolute(given, error);
    if (error) full = given;
    const std::u8string clean = full.lexically_normal().u8string();
    return std::string(clean.begin(), clean.end());
}

/// 資料夾裡的一項：是圖片檔就加進清單（userdata 是那個清單）。
SDL_EnumerationResult SDLCALL collect_image(void* userdata, const char* dirname, const char* name) {
    auto* found = static_cast<std::vector<std::string>*>(userdata);
    if (name == nullptr || name[0] == '\0' || name[0] == '.') return SDL_ENUM_CONTINUE;   // 名稱以點開頭的是隱藏的檔案
    std::string path = dirname != nullptr ? dirname : "";
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += name;
    if (!ImageSequence::is_image(path)) return SDL_ENUM_CONTINUE;
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return SDL_ENUM_CONTINUE;   // 名字像圖片的資料夾不算
    if (has_hidden_attribute(path)) return SDL_ENUM_CONTINUE;   // Windows 的「隱藏」是檔案的屬性，不看檔名
    found->push_back(std::move(path));
    return SDL_ENUM_CONTINUE;
}

}  // namespace

const std::vector<std::string>& ImageSequence::extensions() {
    // 自己讀得進來（stb_image）、平常也真的會遇到的那幾種。HEIC、TIFF、WebP、相機的 RAW 這裡讀不了
    static const std::vector<std::string> list = {"jpg", "jpeg", "jpe", "jfif", "png", "bmp", "gif"};
    return list;
}

bool ImageSequence::is_image(const std::string& path) {
    const std::size_t dot = path.find_last_of("./\\");
    if (dot == std::string::npos || path[dot] != '.' || dot + 1 >= path.size()) return false;
    std::string extension = path.substr(dot + 1);
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return std::find(extensions().begin(), extensions().end(), extension) != extensions().end();
}

bool ImageSequence::natural_less(const std::string& a, const std::string& b) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto fold = [](char c) { return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            // 兩邊都走到一串數字：照數值比。前面的 0 不算；位數少的比較小，位數一樣就一位一位比
            while (i < a.size() && a[i] == '0') ++i;
            while (j < b.size() && b[j] == '0') ++j;
            std::size_t end_a = i, end_b = j;
            while (end_a < a.size() && digit(a[end_a])) ++end_a;
            while (end_b < b.size() && digit(b[end_b])) ++end_b;
            if (end_a - i != end_b - j) return end_a - i < end_b - j;
            const int order = a.compare(i, end_a - i, b, j, end_b - j);
            if (order != 0) return order < 0;
            i = end_a;
            j = end_b;
            continue;
        }
        const unsigned char ca = fold(a[i]), cb = fold(b[j]);
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    if (i < a.size() || j < b.size()) return j < b.size();   // 一邊用完了：短的在前
    return a < b;   // 只差在大小寫或數字前面的 0：照原樣比，每次排出來的順序才一樣
}

std::vector<std::string> ImageSequence::files(const std::vector<std::string>& selection) {
    std::vector<std::string> found;
    for (const std::string& path : selection) {
        SDL_PathInfo info{};
        if (path.empty() || !SDL_GetPathInfo(path.c_str(), &info)) continue;   // 不存在的略過
        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            SDL_EnumerateDirectory(path.c_str(), collect_image, &found);
        } else if (info.type == SDL_PATHTYPE_FILE && is_image(path)) {
            found.push_back(path);
        }
    }
    for (std::string& path : found) path = normalized_path(path);
    // 順序照整個路徑（同一個資料夾裡就是照檔名）；排好之後同一個檔案會排在一起，只留一個
    std::sort(found.begin(), found.end(), natural_less);
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

SourceFramePtr ImageSequence::load(const std::string& path) {
    const SourceFramePtr full = load_image_file(path);
    if (!full || full->width <= 0 || full->height <= 0) return nullptr;
    // 很扁或很長的圖（全景）：照短邊算，長邊跟著放大；不超過原圖，也不超過 8192
    const int longer = std::max(full->width, full->height), shorter = std::min(full->width, full->height);
    const double needed = std::ceil(static_cast<double>(kShortSidePixels) * longer / shorter);
    const int wanted = static_cast<int>(std::min({static_cast<double>(longer), 8192.0, needed}));
    if (wanted >= longer) return full;   // 小圖不放大
    const double ratio = static_cast<double>(wanted) / longer;
    const int width = full->width >= full->height ? wanted : std::max(1, static_cast<int>(std::lround(full->width * ratio)));
    const int height = full->height > full->width ? wanted : std::max(1, static_cast<int>(std::lround(full->height * ratio)));
    const std::shared_ptr<SourceFrame> small = shrink_frame(*full, width, height);
    small->native_width = full->shown_width();   // 畫面上寫的「來源 4032×3024」是原圖的大小
    small->native_height = full->shown_height();
    return small;
}

SourceFramePtr ImageSequence::picture(const std::string& path) {
    for (std::size_t i = 0; i < recent_.size(); ++i) {
        if (recent_[i].first != path) continue;
        std::pair<std::string, SourceFramePtr> hit = std::move(recent_[i]);
        recent_.erase(recent_.begin() + static_cast<std::ptrdiff_t>(i));
        recent_.push_back(std::move(hit));
        return recent_.back().second;
    }
    SourceFramePtr loaded = load(path);
    if (!loaded) return nullptr;
    recent_.emplace_back(path, loaded);
    if (recent_.size() > kKeep) recent_.erase(recent_.begin(), recent_.end() - static_cast<std::ptrdiff_t>(kKeep));
    return loaded;
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
    // 畫面最後要縮成 96×72（大的也只有 192×144），不需要鏡頭最大的解析度：挑一個夠用（至少 384×288）而最小的格式，轉換起來省事
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

/// 和用的那個 ffmpeg 放在同一個目錄的 ffprobe；沒有的話是空字串。不到別處找：Windows 的下載包只附了 ffmpeg，
/// 配上電腦上另外裝的 ffprobe 的話兩個的版本對不起來。沒有 ffprobe 時改問 ffmpeg 本身（見 probe_video）。
std::string ffprobe_beside_ffmpeg() {
    const std::string ffmpeg = FileSource::ffmpeg_path();
#if defined(_WIN32)
    const std::size_t slash = ffmpeg.find_last_of("/\\");
    const char* name = "ffprobe.exe";
#else
    const std::size_t slash = ffmpeg.rfind('/');
    const char* name = "ffprobe";
#endif
    if (slash == std::string::npos) return {};
    const std::string candidate = ffmpeg.substr(0, slash + 1) + name;
    SDL_PathInfo info{};
    return SDL_GetPathInfo(candidate.c_str(), &info) && info.type == SDL_PATHTYPE_FILE ? candidate : std::string();
}

/// 影片多長、畫面多大。
VideoInfo probe_video(const std::string& path) {
    VideoInfo info;
    const std::string ffprobe = ffprobe_beside_ffmpeg();
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
        // 一定要 8 位元的 RGB：10 位元的影片（iPhone 的 HDR、ProRes）不指定的話，ffmpeg 送來的是每個顏色 16 位元的 PPM，read_ppm 讀不了。
        // 改了這裡的參數，apps/desktop/ffmpeg/check.sh 也要照著改（它用同樣的參數檢查內附的 ffmpeg 讀不讀得了各種影片）
        options.args.insert(options.args.end(), {"-pix_fmt", "rgb24", "-f", "image2pipe", "-c:v", "ppm", "-"});
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
