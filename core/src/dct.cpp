#include "dct.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace nbtv {

namespace {

// 正交歸一的 DCT-II 基底：basis[k·n + x] = α_k·cos((2x+1)kπ/(2n))
std::vector<float> make_basis(int n) {
    std::vector<float> b(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        const double alpha = std::sqrt((k == 0 ? 1.0 : 2.0) / n);
        for (int x = 0; x < n; ++x) {
            b[static_cast<std::size_t>(k * n + x)] = static_cast<float>(alpha * std::cos((2 * x + 1) * k * kPi / (2.0 * n)));
        }
    }
    return b;
}

const std::vector<float>& basis_w() {
    static const std::vector<float> b = make_basis(kImageWidth);
    return b;
}

const std::vector<float>& basis_h() {
    static const std::vector<float> b = make_basis(kImageHeight);
    return b;
}

// 一個平面的可分離轉換：先對每一列（水平），再對每一行（垂直）。forward = false 時用轉置的基底。
void transform_plane(const float* in, float* out, bool forward) {
    const std::vector<float>& bw = basis_w();
    const std::vector<float>& bh = basis_h();
    std::vector<float> tmp(static_cast<std::size_t>(kImagePixels));
    for (int y = 0; y < kImageHeight; ++y) {
        const float* row = in + y * kImageWidth;
        for (int u = 0; u < kImageWidth; ++u) {
            float acc = 0.0f;
            if (forward) {
                const float* b = &bw[static_cast<std::size_t>(u * kImageWidth)];
                for (int x = 0; x < kImageWidth; ++x) acc += b[x] * row[x];
            } else {
                for (int k = 0; k < kImageWidth; ++k) acc += bw[static_cast<std::size_t>(k * kImageWidth + u)] * row[k];
            }
            tmp[static_cast<std::size_t>(y * kImageWidth + u)] = acc;
        }
    }
    for (int u = 0; u < kImageWidth; ++u) {
        for (int v = 0; v < kImageHeight; ++v) {
            float acc = 0.0f;
            if (forward) {
                const float* b = &bh[static_cast<std::size_t>(v * kImageHeight)];
                for (int y = 0; y < kImageHeight; ++y) acc += b[y] * tmp[static_cast<std::size_t>(y * kImageWidth + u)];
            } else {
                for (int k = 0; k < kImageHeight; ++k) {
                    acc += bh[static_cast<std::size_t>(k * kImageHeight + v)] * tmp[static_cast<std::size_t>(k * kImageWidth + u)];
                }
            }
            out[v * kImageWidth + u] = acc;
        }
    }
}

}  // namespace

void rgb_to_ycbcr(std::span<const std::uint8_t> rgb, std::span<float> planes) {
    assert(rgb.size() == static_cast<std::size_t>(kImageRgbBytes) && planes.size() == static_cast<std::size_t>(kCoefficients));
    for (int i = 0; i < kImagePixels; ++i) {
        const float r = rgb[static_cast<std::size_t>(3 * i)], g = rgb[static_cast<std::size_t>(3 * i + 1)], b = rgb[static_cast<std::size_t>(3 * i + 2)];
        planes[static_cast<std::size_t>(i)] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
        planes[static_cast<std::size_t>(kImagePixels + i)] = -0.168736f * r - 0.331264f * g + 0.5f * b;
        planes[static_cast<std::size_t>(2 * kImagePixels + i)] = 0.5f * r - 0.418688f * g - 0.081312f * b;
    }
}

void ycbcr_to_rgb(std::span<const float> planes, std::span<std::uint8_t> rgb) {
    assert(rgb.size() == static_cast<std::size_t>(kImageRgbBytes) && planes.size() == static_cast<std::size_t>(kCoefficients));
    auto to_byte = [](float v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    for (int i = 0; i < kImagePixels; ++i) {
        const float y = planes[static_cast<std::size_t>(i)] + 128.0f;
        const float cb = planes[static_cast<std::size_t>(kImagePixels + i)];
        const float cr = planes[static_cast<std::size_t>(2 * kImagePixels + i)];
        rgb[static_cast<std::size_t>(3 * i)] = to_byte(y + 1.402f * cr);
        rgb[static_cast<std::size_t>(3 * i + 1)] = to_byte(y - 0.344136f * cb - 0.714136f * cr);
        rgb[static_cast<std::size_t>(3 * i + 2)] = to_byte(y + 1.772f * cb);
    }
}

void dct_forward(std::span<const float> planes, std::span<float> coefficients) {
    assert(planes.size() == static_cast<std::size_t>(kCoefficients) && coefficients.size() == planes.size());
    for (int p = 0; p < kPlanes; ++p) transform_plane(planes.data() + p * kImagePixels, coefficients.data() + p * kImagePixels, true);
}

void dct_inverse(std::span<const float> coefficients, std::span<float> planes) {
    assert(planes.size() == static_cast<std::size_t>(kCoefficients) && coefficients.size() == planes.size());
    for (int p = 0; p < kPlanes; ++p) transform_plane(coefficients.data() + p * kImagePixels, planes.data() + p * kImagePixels, false);
}

}  // namespace nbtv
