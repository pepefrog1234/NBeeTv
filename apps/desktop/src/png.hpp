// 寫 PNG 檔（stb_image_write）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nbtvgui {

/// pixels 是由左上角開始逐列排列的像素，每個像素 channels 個位元組（3：RGB，4：RGBA）。
bool write_png(const std::string& path, const std::uint8_t* pixels, int width, int height, int channels = 3);

/// 編成 PNG 的位元組（不寫檔）。失敗時是空的。
std::vector<std::uint8_t> encode_png(const std::uint8_t* pixels, int width, int height, int channels = 3);

}  // namespace nbtvgui
