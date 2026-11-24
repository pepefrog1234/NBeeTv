// 學習式網路共用的零件：權重檔的解析，以及卷積、線性層、GELU 這些前向計算的基本運算。
// 畫面的編解碼（learned_codec.cpp）與接收端合併最近幾幀用的小網路（learned_merge.cpp）都用這一份。
// 只給核心裡面的這幾個 .cpp 引用，不是對外的介面。全部是行內函式，不開執行緒。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bits.hpp"
#include "types.hpp"

namespace nbtv::nn {

// 潛在張量的格子：畫面縮小 8 倍
inline constexpr int kGridH = kImageHeight / 8;  // 9
inline constexpr int kGridW = kImageWidth / 8;   // 12

// 擋掉壞掉的檔案要求的巨大配置
inline constexpr std::uint32_t kMaxLatent = 1024;
inline constexpr std::uint32_t kMaxWidth = 1024;
inline constexpr std::uint32_t kMaxTensors = 4096;
inline constexpr std::uint64_t kMaxTensorValues = std::uint64_t{1} << 24;

// ---------------------------------------------------------------- 權重檔

struct RawTensor {
    std::vector<std::uint32_t> dims;
    std::vector<float> data;  // PyTorch 的排列
};

inline std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// IEEE 754 半精度 → 單精度
inline float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {  // 次正規數：把最高位移到隱含的 1 的位置
            std::uint32_t shift = 0;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            bits = sign | ((113u - shift) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);  // 無限大或非數值
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// 依序讀取、每次都檢查剩餘長度
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos = 0;
    bool ok = true;

    const std::uint8_t* take(std::size_t n) {
        if (!ok || n > bytes.size() - pos) {
            ok = false;
            return nullptr;
        }
        const std::uint8_t* p = bytes.data() + pos;
        pos += n;
        return p;
    }
    std::uint32_t u32() {
        const std::uint8_t* p = take(4);
        return p != nullptr ? le32(p) : 0;
    }
    std::uint16_t u16() {
        const std::uint8_t* p = take(2);
        return p != nullptr ? static_cast<std::uint16_t>(p[0] | (p[1] << 8)) : std::uint16_t{0};
    }
    std::uint8_t u8() {
        const std::uint8_t* p = take(1);
        return p != nullptr ? p[0] : std::uint8_t{0};
    }
};

// 魔術字之後的六個數。第二個與最後一個的意義由各種檔案自己定。
struct FileHeader {
    std::uint32_t version = 0;
    std::uint32_t structure = 0;  // 畫面的模型：結構編號
    std::uint32_t latent = 0;     // 潛在通道數
    std::uint32_t count = 0;      // 張量的個數
    std::uint32_t dtype = 0;      // 0：32 位元浮點數；1：16 位元
    std::uint32_t extra = 0;      // 畫面的模型：保留（0）
};

// 解析一個權重檔：魔術字、標頭、各個張量、結尾的 CRC-32（交給 id）。格式寫在 tools/learned/export.py 開頭。
// wrong_kind 是魔術字不對時的訊息。accept 在讀完標頭之後呼叫，讓各種檔案自己檢查版本與結構；
// 它回傳 false 時要把原因填進第二個參數。
template <typename Accept>
bool parse_file(std::span<const std::uint8_t> file, const char (&magic)[8], const char* wrong_kind, Accept accept,
                std::map<std::string, RawTensor>& tensors, FileHeader& header, std::uint32_t& id, std::string& error) {
    if (file.size() < 36 || std::memcmp(file.data(), magic, 8) != 0) {
        error = wrong_kind;
        return false;
    }
    const std::size_t body = file.size() - 4;
    id = le32(file.data() + body);
    if (crc32(file.first(body)) != id) {
        error = "權重檔的檢查碼不符，檔案可能損壞";
        return false;
    }
    Reader r{file.first(body), 8};
    header.version = r.u32();
    header.structure = r.u32();
    header.latent = r.u32();
    header.count = r.u32();
    header.dtype = r.u32();
    header.extra = r.u32();
    if (!accept(header, error)) return false;
    if (header.dtype > 1 || header.latent == 0 || header.latent > kMaxLatent || header.count > kMaxTensors) {
        error = "權重檔的標頭不合理";
        return false;
    }
    const std::size_t width = header.dtype == 1 ? 2 : 4;
    for (std::uint32_t t = 0; t < header.count; ++t) {
        const std::size_t name_length = r.u16();
        const std::uint8_t* name_bytes = r.take(name_length);
        const std::uint32_t ndim = r.u8();
        if (!r.ok || ndim == 0 || ndim > 4) {
            error = "權重檔裡張量的描述不完整";
            return false;
        }
        RawTensor tensor;
        std::uint64_t total = 1;
        for (std::uint32_t d = 0; d < ndim; ++d) {
            const std::uint32_t size = r.u32();
            if (!r.ok || size == 0 || size > kMaxTensorValues) {
                error = "權重檔裡張量的大小不合理";
                return false;
            }
            total *= size;  // 乘之前兩邊都不超過 2^24，不會溢位
            if (total > kMaxTensorValues) {
                error = "權重檔裡張量的大小不合理";
                return false;
            }
            tensor.dims.push_back(size);
        }
        const std::uint8_t* data = r.take(static_cast<std::size_t>(total) * width);
        if (data == nullptr) {
            error = "權重檔的長度不足";
            return false;
        }
        tensor.data.resize(static_cast<std::size_t>(total));
        for (std::size_t i = 0; i < tensor.data.size(); ++i) {
            float v = 0.0f;
            if (header.dtype == 1) {
                v = half_to_float(static_cast<std::uint16_t>(data[2 * i] | (data[2 * i + 1] << 8)));
            } else {
                const std::uint32_t bits = le32(data + 4 * i);
                std::memcpy(&v, &bits, sizeof v);
            }
            if (!std::isfinite(v)) {
                error = "權重檔裡有無限大或非數值";
                return false;
            }
            tensor.data[i] = v;
        }
        std::string name(reinterpret_cast<const char*>(name_bytes), name_length);
        if (!tensors.emplace(std::move(name), std::move(tensor)).second) {
            error = "權重檔裡有重複的張量名稱";
            return false;
        }
    }
    if (!r.ok || r.pos != body) {
        error = "權重檔的長度和內容對不上";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 網路的零件

// 特徵圖：高 × 寬 × 通道，通道在最內層。卷積用到的一列相鄰像素因此是一段連續的記憶體。
struct Map {
    int h = 0, w = 0, c = 0;
    std::vector<float> v;

    void shape(int height, int width, int channels) {
        h = height;
        w = width;
        c = channels;
        v.assign(static_cast<std::size_t>(h) * static_cast<std::size_t>(w) * static_cast<std::size_t>(c), 0.0f);
    }
    std::size_t offset(int y, int x) const {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(c);
    }
    float* at(int y, int x) { return v.data() + offset(y, x); }
    const float* at(int y, int x) const { return v.data() + offset(y, x); }
};

struct Conv {
    int cout = 0, cin = 0, k = 0, stride = 1, pad = 0;
    std::vector<float> w;  // [輸出通道][核心的列][核心的行][輸入通道]
    std::vector<float> b;
};

struct Linear {
    int out = 0, in = 0;
    std::vector<float> w;  // [輸出][輸入]
    std::vector<float> b;
};

struct Block {
    Conv c1, c2;
};

// 內積的累加器分成這麼多路。最內層是固定長度的迴圈，編譯器會把它變成向量運算；
// 分路之後求和的順序是寫死的，不需要讓編譯器重排浮點加法（-ffast-math）。
inline constexpr int kLanes = 16;

inline float sum_lanes(const float* a) {
    float s = 0.0f;
    for (int j = 0; j < kLanes; ++j) s += a[j];
    return s;
}

// 一個輸出像素、四個輸出通道：核心的每一列對應輸入裡一段連續的值，各自和四組權重做內積。
// 四個通道一起算，輸入只需要讀一次。
inline void pixel_dot4(const float* in, std::size_t row_step, const float* w0, const float* w1, const float* w2, const float* w3,
                       int rows, int n, float out[4]) {
    float a0[kLanes] = {}, a1[kLanes] = {}, a2[kLanes] = {}, a3[kLanes] = {};
    for (int r = 0; r < rows; ++r) {
        const float* x = in + static_cast<std::size_t>(r) * row_step;
        const std::size_t off = static_cast<std::size_t>(r) * static_cast<std::size_t>(n);
        const float* p0 = w0 + off;
        const float* p1 = w1 + off;
        const float* p2 = w2 + off;
        const float* p3 = w3 + off;
        int i = 0;
        for (; i + kLanes <= n; i += kLanes) {
            for (int j = 0; j < kLanes; ++j) {
                const float v = x[i + j];
                a0[j] += v * p0[i + j];
                a1[j] += v * p1[i + j];
                a2[j] += v * p2[i + j];
                a3[j] += v * p3[i + j];
            }
        }
        for (; i < n; ++i) {
            const float v = x[i];
            a0[0] += v * p0[i];
            a1[0] += v * p1[i];
            a2[0] += v * p2[i];
            a3[0] += v * p3[i];
        }
    }
    out[0] = sum_lanes(a0);
    out[1] = sum_lanes(a1);
    out[2] = sum_lanes(a2);
    out[3] = sum_lanes(a3);
}

// 輸出通道數不是 4 的倍數時，剩下的一個一個算
inline float pixel_dot1(const float* in, std::size_t row_step, const float* w, int rows, int n) {
    float a[kLanes] = {};
    for (int r = 0; r < rows; ++r) {
        const float* x = in + static_cast<std::size_t>(r) * row_step;
        const float* p = w + static_cast<std::size_t>(r) * static_cast<std::size_t>(n);
        int i = 0;
        for (; i + kLanes <= n; i += kLanes)
            for (int j = 0; j < kLanes; ++j) a[j] += x[i + j] * p[i + j];
        for (; i < n; ++i) a[0] += x[i] * p[i];
    }
    return sum_lanes(a);
}

// 二維卷積。out 不可以和 in 是同一個。
inline void conv2d(const Conv& cv, const Map& in, Map& out) {
    const int ho = (in.h + 2 * cv.pad - cv.k) / cv.stride + 1;
    const int wo = (in.w + 2 * cv.pad - cv.k) / cv.stride + 1;
    const int wp = in.w + 2 * cv.pad;
    const std::size_t cin = static_cast<std::size_t>(cv.cin);
    const std::size_t row_step = static_cast<std::size_t>(wp) * cin;  // 往下一列要跳過幾個值

    // 四周補零的輸入；不補零的卷積（1×1）直接用原來的
    std::vector<float> padded;
    const float* src = in.v.data();
    if (cv.pad > 0) {
        padded.assign(static_cast<std::size_t>(in.h + 2 * cv.pad) * row_step, 0.0f);
        for (int y = 0; y < in.h; ++y) {
            const std::size_t first = static_cast<std::size_t>(y + cv.pad) * static_cast<std::size_t>(wp) + static_cast<std::size_t>(cv.pad);
            std::memcpy(padded.data() + first * cin, in.at(y, 0), static_cast<std::size_t>(in.w) * cin * sizeof(float));
        }
        src = padded.data();
    }

    out.shape(ho, wo, cv.cout);
    const int seg = cv.k * cv.cin;  // 核心的一列對應多長的一段
    const std::size_t per_out = static_cast<std::size_t>(cv.k) * static_cast<std::size_t>(seg);  // 一個輸出通道的權重個數
    const std::size_t px_step = static_cast<std::size_t>(cv.stride) * cin;
    const std::size_t out_c = static_cast<std::size_t>(cv.cout);

    // 輸出通道在外層：四個通道的權重留在快取裡，輸入則是順著掃過去
    int oc = 0;
    for (; oc + 4 <= cv.cout; oc += 4) {
        const float* w0 = cv.w.data() + static_cast<std::size_t>(oc) * per_out;
        const float* w1 = w0 + per_out;
        const float* w2 = w1 + per_out;
        const float* w3 = w2 + per_out;
        const float b0 = cv.b[static_cast<std::size_t>(oc)], b1 = cv.b[static_cast<std::size_t>(oc) + 1];
        const float b2 = cv.b[static_cast<std::size_t>(oc) + 2], b3 = cv.b[static_cast<std::size_t>(oc) + 3];
        for (int oy = 0; oy < ho; ++oy) {
            const float* px = src + static_cast<std::size_t>(oy) * static_cast<std::size_t>(cv.stride) * row_step;
            float* dst = out.at(oy, 0) + oc;
            for (int ox = 0; ox < wo; ++ox, px += px_step, dst += out_c) {
                float sum[4];
                pixel_dot4(px, row_step, w0, w1, w2, w3, cv.k, seg, sum);
                dst[0] = sum[0] + b0;
                dst[1] = sum[1] + b1;
                dst[2] = sum[2] + b2;
                dst[3] = sum[3] + b3;
            }
        }
    }
    for (; oc < cv.cout; ++oc) {
        const float* w = cv.w.data() + static_cast<std::size_t>(oc) * per_out;
        const float bias = cv.b[static_cast<std::size_t>(oc)];
        for (int oy = 0; oy < ho; ++oy) {
            const float* px = src + static_cast<std::size_t>(oy) * static_cast<std::size_t>(cv.stride) * row_step;
            float* dst = out.at(oy, 0) + oc;
            for (int ox = 0; ox < wo; ++ox, px += px_step, dst += out_c) *dst = pixel_dot1(px, row_step, w, cv.k, seg) + bias;
        }
    }
}

// GELU（和 PyTorch 預設的一樣，用誤差函數的精確式）
inline float gelu(float x) { return 0.5f * x * (1.0f + std::erf(x * 0.70710678118654752f)); }

inline void gelu_inplace(std::vector<float>& v) {
    for (float& x : v) x = gelu(x);
}

inline std::vector<float> apply(const Linear& l, const float* x) {
    std::vector<float> y(static_cast<std::size_t>(l.out));
    for (int o = 0; o < l.out; ++o) {
        const float* w = l.w.data() + static_cast<std::size_t>(o) * static_cast<std::size_t>(l.in);
        float s = l.b[static_cast<std::size_t>(o)];
        for (int i = 0; i < l.in; ++i) s += w[i] * x[i];
        y[static_cast<std::size_t>(o)] = s;
    }
    return y;
}

// 殘差塊：x ← x + c2(gelu(調整(c1(gelu(x)))))。
// 「調整」是解碼器依訊雜比做的逐通道縮放與位移（gamma、beta 各 x.c 個）；編碼器沒有，傳空指標。
// t1、t2 是暫存。
inline void res_block(const Block& b, Map& x, const float* gamma, const float* beta, Map& t1, Map& t2) {
    t1 = x;
    gelu_inplace(t1.v);
    conv2d(b.c1, t1, t2);
    if (gamma != nullptr) {
        const std::size_t c = static_cast<std::size_t>(t2.c);
        for (std::size_t i = 0; i < t2.v.size(); i += c)
            for (std::size_t ch = 0; ch < c; ++ch) t2.v[i + ch] = t2.v[i + ch] * (1.0f + gamma[ch]) + beta[ch];
    }
    gelu_inplace(t2.v);
    conv2d(b.c2, t2, t1);
    for (std::size_t i = 0; i < x.v.size(); ++i) x.v[i] += t1.v[i];
}

// 把通道攤成兩倍的解析度：輸入 h×w×4c → 輸出 2h×2w×c（PyTorch 的 PixelShuffle(2)）
inline void pixel_shuffle2(const Map& in, Map& out) {
    const int c = in.c / 4;
    out.shape(in.h * 2, in.w * 2, c);
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) {
            const float* p = in.at(y, x);
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    float* q = out.at(2 * y + i, 2 * x + j);
                    for (int ch = 0; ch < c; ++ch) q[ch] = p[ch * 4 + i * 2 + j];
                }
        }
}

// 從解析好的張量組出模型：逐一核對名稱與形狀
struct Builder {
    const std::map<std::string, RawTensor>& tensors;
    std::string& error;
    std::size_t used = 0;

    const RawTensor* get(const std::string& name, std::initializer_list<int> dims) {
        const auto it = tensors.find(name);
        bool ok = it != tensors.end() && it->second.dims.size() == dims.size();
        if (ok) {
            std::size_t d = 0;
            for (int size : dims) ok = ok && it->second.dims[d++] == static_cast<std::uint32_t>(size);
        }
        if (!ok) {
            if (error.empty()) error = "權重檔缺少張量或形狀不符：" + name;
            return nullptr;
        }
        ++used;
        return &it->second;
    }

    bool conv(const std::string& name, int cout, int cin, int k, int stride, int pad, Conv& out) {
        const RawTensor* w = get(name + ".weight", {cout, cin, k, k});
        const RawTensor* b = get(name + ".bias", {cout});
        if (w == nullptr || b == nullptr) return false;
        out.cout = cout;
        out.cin = cin;
        out.k = k;
        out.stride = stride;
        out.pad = pad;
        out.b = b->data;
        out.w.resize(w->data.size());
        // PyTorch 的排列是 [輸出][輸入][列][行]，這裡要 [輸出][列][行][輸入]
        const std::size_t ks = static_cast<std::size_t>(k), cins = static_cast<std::size_t>(cin);
        std::size_t src = 0;
        for (std::size_t o = 0; o < static_cast<std::size_t>(cout); ++o)
            for (std::size_t i = 0; i < cins; ++i)
                for (std::size_t ky = 0; ky < ks; ++ky)
                    for (std::size_t kx = 0; kx < ks; ++kx) out.w[((o * ks + ky) * ks + kx) * cins + i] = w->data[src++];
        return true;
    }

    bool linear(const std::string& name, int out_size, int in_size, Linear& out) {
        const RawTensor* w = get(name + ".weight", {out_size, in_size});
        const RawTensor* b = get(name + ".bias", {out_size});
        if (w == nullptr || b == nullptr) return false;
        out.out = out_size;
        out.in = in_size;
        out.w = w->data;
        out.b = b->data;
        return true;
    }

    bool block(const std::string& name, int channels, Block& out) {
        return conv(name + ".c1", channels, channels, 3, 1, 1, out.c1) && conv(name + ".c2", channels, channels, 3, 1, 1, out.c2);
    }
};

}  // namespace nbtv::nn
