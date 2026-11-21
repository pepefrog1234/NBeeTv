#include "image_codec.hpp"

#include "av1_payload.hpp"
#include "types.hpp"

#if NBTV_HAVE_AVIF

#include <algorithm>
#include <memory>

#include <avif/avif.h>

namespace nbtv {

namespace {

struct ImageDeleter {
    void operator()(avifImage* p) const { avifImageDestroy(p); }
};
struct EncoderDeleter {
    void operator()(avifEncoder* p) const { avifEncoderDestroy(p); }
};
struct DecoderDeleter {
    void operator()(avifDecoder* p) const { avifDecoderDestroy(p); }
};

using ImagePtr = std::unique_ptr<avifImage, ImageDeleter>;

// RGB → 4:4:4 的 YUV（BT.601 矩陣、全範圍；外殼的 colr 箱也是這樣標示）
ImagePtr make_yuv(std::span<const std::uint8_t> rgb) {
    if (rgb.size() != static_cast<std::size_t>(kImageRgbBytes)) return nullptr;
    ImagePtr image(avifImageCreate(kImageWidth, kImageHeight, 8, AVIF_PIXEL_FORMAT_YUV444));
    if (!image) return nullptr;
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
    image->yuvRange = AVIF_RANGE_FULL;

    avifRGBImage src;
    avifRGBImageSetDefaults(&src, image.get());
    src.format = AVIF_RGB_FORMAT_RGB;
    src.depth = 8;
    src.pixels = const_cast<std::uint8_t*>(rgb.data());  // libavif 只讀取
    src.rowBytes = kImageWidth * 3;
    if (avifImageRGBToYUV(image.get(), &src) != AVIF_RESULT_OK) return nullptr;
    return image;
}

// 以指定的量化參數編成 .avif 檔（失敗時是空的）。
std::vector<std::uint8_t> encode_file(const avifImage* image, int quantizer, int speed) {
    std::unique_ptr<avifEncoder, EncoderDeleter> encoder(avifEncoderCreate());
    if (!encoder) return {};
    encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
    encoder->maxThreads = 1;  // 核心不開執行緒
    encoder->speed = std::clamp(speed, 0, 10);
    // libavif 的 quality 與量化參數一一對應：quality = ((63 − q)·100 + 31)/63
    encoder->quality = ((63 - std::clamp(quantizer, 0, 63)) * 100 + 31) / 63;
    avifRWData output = AVIF_DATA_EMPTY;
    std::vector<std::uint8_t> file;
    if (avifEncoderWrite(encoder.get(), image, &output) == AVIF_RESULT_OK) file.assign(output.data, output.data + output.size);
    avifRWDataFree(&output);
    return file;
}

std::vector<std::uint8_t> encode_once(const avifImage* image, int quantizer, int speed) {
    return payload_from_avif(encode_file(image, quantizer, speed));
}

}  // namespace

bool image_codec_available() { return true; }

bool image_encode_avif_file(std::span<const std::uint8_t> rgb, int quantizer, int speed, std::vector<std::uint8_t>& file) {
    const ImagePtr image = make_yuv(rgb);
    if (!image) return false;
    file = encode_file(image.get(), quantizer, speed);
    return !file.empty();
}

bool image_encode(std::span<const std::uint8_t> rgb, int max_bytes, int speed, ImageEncodeResult& result) {
    result = ImageEncodeResult{};
    if (max_bytes <= 0) return false;
    const ImagePtr image = make_yuv(rgb);
    if (!image) return false;

    // 找「放得下的最小量化參數」。量化愈粗檔案愈小，所以可以二分搜尋。
    int lo = 0, hi = 63;
    bool have = false;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        std::vector<std::uint8_t> payload = encode_once(image.get(), mid, speed);
        ++result.encodes;
        if (!payload.empty() && static_cast<int>(payload.size()) <= max_bytes) {
            result.payload = std::move(payload);
            result.quantizer = mid;
            have = true;
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (!have || result.quantizer != lo) {
        // 搜尋收斂到一個還沒試過的值（只會是最粗的 63）
        std::vector<std::uint8_t> payload = encode_once(image.get(), lo, speed);
        ++result.encodes;
        if (payload.empty() || static_cast<int>(payload.size()) > max_bytes) return have;
        result.payload = std::move(payload);
        result.quantizer = lo;
    }
    return true;
}

bool image_decode(std::span<const std::uint8_t> payload, std::span<std::uint8_t> rgb) {
    if (rgb.size() != static_cast<std::size_t>(kImageRgbBytes)) return false;
    const std::vector<std::uint8_t> file = avif_from_payload(payload);
    if (file.empty()) return false;

    std::unique_ptr<avifDecoder, DecoderDeleter> decoder(avifDecoderCreate());
    std::unique_ptr<avifImage, ImageDeleter> image(avifImageCreateEmpty());
    if (!decoder || !image) return false;
    decoder->maxThreads = 1;
    if (avifDecoderReadMemory(decoder.get(), image.get(), file.data(), file.size()) != AVIF_RESULT_OK) return false;
    if (image->width != static_cast<std::uint32_t>(kImageWidth) || image->height != static_cast<std::uint32_t>(kImageHeight)) return false;

    avifRGBImage dst;
    avifRGBImageSetDefaults(&dst, image.get());
    dst.format = AVIF_RGB_FORMAT_RGB;
    dst.depth = 8;
    dst.pixels = rgb.data();
    dst.rowBytes = kImageWidth * 3;
    return avifImageYUVToRGB(image.get(), &dst) == AVIF_RESULT_OK;
}

}  // namespace nbtv

#else  // 沒有 libavif

namespace nbtv {

bool image_codec_available() { return false; }
bool image_encode(std::span<const std::uint8_t>, int, int, ImageEncodeResult& result) {
    result = ImageEncodeResult{};
    return false;
}
bool image_decode(std::span<const std::uint8_t>, std::span<std::uint8_t>) { return false; }
bool image_encode_avif_file(std::span<const std::uint8_t>, int, int, std::vector<std::uint8_t>&) { return false; }

}  // namespace nbtv

#endif
