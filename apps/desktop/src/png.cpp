#include "png.hpp"

#include <cstdio>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_STATIC
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image_write.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace nbtvgui {

std::vector<std::uint8_t> encode_png(const std::uint8_t* pixels, int width, int height, int channels) {
    std::vector<std::uint8_t> bytes;
    const auto collect = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<std::uint8_t>*>(context);
        const auto* begin = static_cast<const std::uint8_t*>(data);
        out->insert(out->end(), begin, begin + size);
    };
    if (stbi_write_png_to_func(collect, &bytes, width, height, channels, pixels, width * channels) == 0) bytes.clear();
    return bytes;
}

std::vector<std::uint8_t> encode_jpeg(const std::uint8_t* pixels, int width, int height, int quality) {
    std::vector<std::uint8_t> bytes;
    const auto collect = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<std::uint8_t>*>(context);
        const auto* begin = static_cast<const std::uint8_t*>(data);
        out->insert(out->end(), begin, begin + size);
    };
    if (stbi_write_jpg_to_func(collect, &bytes, width, height, 3, pixels, quality) == 0) bytes.clear();
    return bytes;
}

bool write_png(const std::string& path, const std::uint8_t* pixels, int width, int height, int channels) {
    const std::vector<std::uint8_t> bytes = encode_png(pixels, width, height, channels);
    if (bytes.empty()) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

}  // namespace nbtvgui
