// 測試用的三張 96×72 畫面（咖啡、火箭、貓，都帶「CQ TEST 73」字幕），取自介面版面文件裡的示意圖。
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ppm.hpp"
#include "types.hpp"

namespace test_images {

inline std::vector<std::uint8_t> load(const char* name) {
    std::vector<std::uint8_t> rgb;
    int w = 0, h = 0;
    const std::string path = std::string(NBTV_TEST_DATA_DIR) + "/" + name + ".ppm";
    if (!nbtv::tools::read_ppm(path, rgb, &w, &h) || w != nbtv::kImageWidth || h != nbtv::kImageHeight) {
        std::printf("讀不到測試圖 %s\n", path.c_str());
        rgb.assign(nbtv::kImageRgbBytes, 128);
    }
    return rgb;
}

// 同一張測試圖放大成 width×height：每個像素重複整數倍。畫面比較大的模型（模式 4–7 與 15 的 192×144）用；
// tools/learned/export.py 產生參考答案時做的是同一件事。96×72 就是原圖；不是 96×72 的整數倍時回傳灰色的畫面。
inline std::vector<std::uint8_t> load(const char* name, int width, int height) {
    const std::vector<std::uint8_t> small = load(name);
    const int times = width / nbtv::kImageWidth;
    if (times == 1 && height == nbtv::kImageHeight) return small;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 128);
    if (times < 1 || width != nbtv::kImageWidth * times || height != nbtv::kImageHeight * times) {
        std::printf("測試圖放大不成 %d×%d\n", width, height);
        return rgb;
    }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c)
                rgb[static_cast<std::size_t>(3 * (y * width + x) + c)] =
                    small[static_cast<std::size_t>(3 * ((y / times) * nbtv::kImageWidth + x / times) + c)];
    return rgb;
}

inline const char* const kNames[] = {"coffee", "rocket", "cat"};

// RGB 的峰值訊雜比（dB）
inline double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double mse = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        mse += d * d;
    }
    mse /= static_cast<double>(a.size());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace test_images
