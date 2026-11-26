// 命令列工具用的影像檔讀寫：二進位 PPM（P6）。這裡什麼大小都讀；各個工具自己檢查是不是那個模式的畫面大小
// （模式 11–14 是 96×72，模式 5–7 與 15 是 192×144）。其他格式請先用 ffmpeg 或 sips 轉成 PPM。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nbtv::tools {

inline bool read_ppm(const std::string& path, std::vector<std::uint8_t>& rgb, int* width = nullptr, int* height = nullptr) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    auto next_int = [&]() -> int {
        int c = std::fgetc(f);
        for (;;) {
            while (c == ' ' || c == '\n' || c == '\r' || c == '\t') c = std::fgetc(f);
            if (c != '#') break;
            while (c != '\n' && c != EOF) c = std::fgetc(f);
        }
        int v = 0;
        bool any = false;
        while (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            any = true;
            c = std::fgetc(f);
        }
        return any ? v : -1;
    };
    bool ok = std::fgetc(f) == 'P' && std::fgetc(f) == '6';
    const int w = ok ? next_int() : -1;
    const int h = ok ? next_int() : -1;
    const int maxval = ok ? next_int() : -1;
    ok = ok && w > 0 && h > 0 && w <= 8192 && h <= 8192 && maxval == 255;
    if (ok) {
        rgb.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
        ok = std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    }
    std::fclose(f);
    if (width != nullptr) *width = w;
    if (height != nullptr) *height = h;
    return ok;
}

inline bool write_ppm(const std::string& path, const std::vector<std::uint8_t>& rgb, int width, int height) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    std::fclose(f);
    return ok;
}

}  // namespace nbtv::tools
