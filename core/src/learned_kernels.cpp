#include "learned_kernels.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if (defined(__x86_64__) || defined(_M_X64)) && defined(_MSC_VER) && !defined(__clang__)
#include <immintrin.h>
#include <intrin.h>
#endif

namespace nbtv::nn {

namespace {

// ---------------------------------------------------------------- 一般的寫法

// 內積的累加器分成 16 路（kLanes）：核心每一列的第 i 個值加進第 i mod 16 路，每一列都從第 0 路排起。
// 最內層是固定長度的迴圈，編譯器會把它變成向量運算；分路之後求和的順序是寫死的，
// 不需要讓編譯器重排浮點加法（-ffast-math）。avx2 的版本照同樣的分法與順序，只差在乘加合成一個運算。
constexpr int kLanes = 16;

// 十六路加成一個數：兩兩相加四次（8、4、2、1）。
inline float sum_lanes(const float* a) {
    float b[8], c[4];
    for (int j = 0; j < 8; ++j) b[j] = a[j] + a[j + 8];
    for (int j = 0; j < 4; ++j) c[j] = b[j] + b[j + 4];
    return (c[0] + c[2]) + (c[1] + c[3]);
}

// 核心的一列不是 16 的倍數時（編碼器的第一層一列是 15 個值，解碼器的第一層是潛在通道數），
// 剩下的那幾個值補零湊成 16 個再算，才不必一個一個來。權重的那一份每四個輸出通道補一次，
// 核心最多 kConvTailRows 列；更多列的走一個一個算的那條路，結果一樣。
struct Tails4 {
    float w[4][kConvTailRows][kLanes];
};

constexpr std::int32_t kKeepLanes[2 * kLanes] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // 後面 16 個是 0

// 一個輸出像素、四個輸出通道：核心的每一列對應輸入裡一段連續的值，各自和四組權重做內積。
// 四個通道一起算，輸入只需要讀一次。這一個給「一列剛好是 16 的倍數」的卷積用（模型裡絕大部分的運算量）；
// 另外寫成一個函式，最內層才會是編譯器看得懂的單純迴圈。
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
        for (int i = 0; i + kLanes <= n; i += kLanes) {
            for (int j = 0; j < kLanes; ++j) {
                const float v = x[i + j];
                a0[j] += v * p0[i + j];
                a1[j] += v * p1[i + j];
                a2[j] += v * p2[i + j];
                a3[j] += v * p3[i + j];
            }
        }
    }
    out[0] = sum_lanes(a0);
    out[1] = sum_lanes(a1);
    out[2] = sum_lanes(a2);
    out[3] = sum_lanes(a3);
}

// 同上，一列不是 16 的倍數時用：尾巴那幾個值補零湊成 16 個（補的那幾路加上去的是 0，累加器不變）。
// tails 是四組權重每一列的尾巴補零後的樣子。
inline void pixel_dot4_ragged(const float* in, std::size_t row_step, const float* w0, const float* w1, const float* w2, const float* w3,
                              const Tails4& tails, int rows, int n, float out[4]) {
    float a0[kLanes] = {}, a1[kLanes] = {}, a2[kLanes] = {}, a3[kLanes] = {};
    const int rem = n % kLanes, full = n - rem;
    const std::int32_t* keep = kKeepLanes + kLanes - rem;  // 前 rem 個是 1，後面是 0
    for (int r = 0; r < rows; ++r) {
        const float* x = in + static_cast<std::size_t>(r) * row_step;
        const std::size_t off = static_cast<std::size_t>(r) * static_cast<std::size_t>(n);
        const float* p0 = w0 + off;
        const float* p1 = w1 + off;
        const float* p2 = w2 + off;
        const float* p3 = w3 + off;
        for (int i = 0; i < full; i += kLanes) {
            for (int j = 0; j < kLanes; ++j) {
                const float v = x[i + j];
                a0[j] += v * p0[i + j];
                a1[j] += v * p1[i + j];
                a2[j] += v * p2[i + j];
                a3[j] += v * p3[i + j];
            }
        }
        // 輸入這邊直接讀 16 個（後面多讀到的由 keep 換成 0；ConvTile 保證讀得到），不另外抄一份
        const float* xs = x + full;
        const float* q0 = tails.w[0][r];
        const float* q1 = tails.w[1][r];
        const float* q2 = tails.w[2][r];
        const float* q3 = tails.w[3][r];
        for (int j = 0; j < kLanes; ++j) {
            const float read = xs[j];  // 先讀再挑：讀本身沒有條件，編譯器才能把這個迴圈變成向量運算
            const float v = keep[j] != 0 ? read : 0.0f;
            a0[j] += v * q0[j];
            a1[j] += v * q1[j];
            a2[j] += v * q2[j];
            a3[j] += v * q3[j];
        }
    }
    out[0] = sum_lanes(a0);
    out[1] = sum_lanes(a1);
    out[2] = sum_lanes(a2);
    out[3] = sum_lanes(a3);
}

// 一個輸出通道（輸出通道數不是 4 的倍數時剩下的，以及核心的列數太多、沒辦法補零的）：尾巴一個值一路，結果和補零的算法一樣
inline float pixel_dot1(const float* in, std::size_t row_step, const float* w, int rows, int n) {
    float a[kLanes] = {};
    const int rem = n % kLanes, full = n - rem;
    for (int r = 0; r < rows; ++r) {
        const float* x = in + static_cast<std::size_t>(r) * row_step;
        const float* p = w + static_cast<std::size_t>(r) * static_cast<std::size_t>(n);
        for (int i = 0; i < full; i += kLanes)
            for (int j = 0; j < kLanes; ++j) a[j] += x[i + j] * p[i + j];
        for (int j = 0; j < rem; ++j) a[j] += x[full + j] * p[full + j];
    }
    return sum_lanes(a);
}

void conv_tile_generic(const ConvTile& t) {
    const std::size_t per_out = static_cast<std::size_t>(t.k) * static_cast<std::size_t>(t.seg);  // 一個輸出通道的權重個數
    const std::size_t out_c = static_cast<std::size_t>(t.cout);
    const std::size_t out_row = static_cast<std::size_t>(t.wo) * out_c;
    const std::size_t in_row = static_cast<std::size_t>(t.stride) * t.row_step;
    const int rem = t.seg % kLanes, full = t.seg - rem;

    // 輸出通道在外層：四個通道的權重留在快取裡，輸入則是順著掃過去
    int oc = t.oc_begin;
    if (rem == 0) {
        for (; oc + 4 <= t.oc_end; oc += 4) {
            const float* w0 = t.weights + static_cast<std::size_t>(oc) * per_out;
            const float* w1 = w0 + per_out;
            const float* w2 = w1 + per_out;
            const float* w3 = w2 + per_out;
            const float b0 = t.bias[oc], b1 = t.bias[oc + 1], b2 = t.bias[oc + 2], b3 = t.bias[oc + 3];
            for (int oy = t.oy_begin; oy < t.oy_end; ++oy) {
                const float* px = t.src + static_cast<std::size_t>(oy) * in_row;
                float* dst = t.dst + static_cast<std::size_t>(oy) * out_row + static_cast<std::size_t>(oc);
                for (int ox = 0; ox < t.wo; ++ox, px += t.px_step, dst += out_c) {
                    float sum[4];
                    pixel_dot4(px, t.row_step, w0, w1, w2, w3, t.k, t.seg, sum);
                    dst[0] = sum[0] + b0;
                    dst[1] = sum[1] + b1;
                    dst[2] = sum[2] + b2;
                    dst[3] = sum[3] + b3;
                }
            }
        }
    } else if (t.k <= kConvTailRows) {
        for (; oc + 4 <= t.oc_end; oc += 4) {
            const float* w[4];
            Tails4 tails;
            std::memset(&tails, 0, sizeof(tails));
            for (int c = 0; c < 4; ++c) {
                w[c] = t.weights + static_cast<std::size_t>(oc + c) * per_out;
                for (int r = 0; r < t.k; ++r)
                    std::memcpy(tails.w[c][r], w[c] + static_cast<std::size_t>(r) * static_cast<std::size_t>(t.seg) + static_cast<std::size_t>(full),
                                static_cast<std::size_t>(rem) * sizeof(float));
            }
            const float b0 = t.bias[oc], b1 = t.bias[oc + 1], b2 = t.bias[oc + 2], b3 = t.bias[oc + 3];
            for (int oy = t.oy_begin; oy < t.oy_end; ++oy) {
                const float* px = t.src + static_cast<std::size_t>(oy) * in_row;
                float* dst = t.dst + static_cast<std::size_t>(oy) * out_row + static_cast<std::size_t>(oc);
                for (int ox = 0; ox < t.wo; ++ox, px += t.px_step, dst += out_c) {
                    float sum[4];
                    pixel_dot4_ragged(px, t.row_step, w[0], w[1], w[2], w[3], tails, t.k, t.seg, sum);
                    dst[0] = sum[0] + b0;
                    dst[1] = sum[1] + b1;
                    dst[2] = sum[2] + b2;
                    dst[3] = sum[3] + b3;
                }
            }
        }
    }
    for (; oc < t.oc_end; ++oc) {
        const float* w = t.weights + static_cast<std::size_t>(oc) * per_out;
        const float bias = t.bias[oc];
        for (int oy = t.oy_begin; oy < t.oy_end; ++oy) {
            const float* px = t.src + static_cast<std::size_t>(oy) * in_row;
            float* dst = t.dst + static_cast<std::size_t>(oy) * out_row + static_cast<std::size_t>(oc);
            for (int ox = 0; ox < t.wo; ++ox, px += t.px_step, dst += out_c) *dst = pixel_dot1(px, t.row_step, w, t.k, t.seg) + bias;
        }
    }
}

// GELU(x) = 0.5·x·(1 + erf(x/√2))，PyTorch 預設的那一種。
//
// 誤差函數不用 C 函式庫的 erf：各家的實作速度差很多（一張畫面要算幾百萬次），結果也有末位的差異。
// 這裡用一個有理式 erf(z) ≈ z·P(z²)/Q(z²)，在 |z| ≤ 4 上和真正的 erf 最多差 7.5e-10（係數是自己擬合的，
// 擬合的做法與誤差在 core/tests/test_kernels.cpp 裡對照 std::erf 檢查）；|z| ≥ 4 時 erf 和 ±1 的差不到 1.6e-8，
// 直接當成 ±1，所以很負的輸入得到的是精確的 0。整個式子用雙精度算，最後才轉回單精度：
// 結果和「用精確的 erf、全程雙精度」算出來的最多差單精度的一個末位。
// avx2 的版本（learned_kernels_avx2.cpp）用的是同一組係數、同樣的順序。
void gelu_generic(float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const double x = static_cast<double>(values[i]);
        double z = x * kInvSqrt2;
        z = z < -kErfLimit ? -kErfLimit : (z > kErfLimit ? kErfLimit : z);  // 非數值照樣傳下去
        const double t = z * z;
        double p = kErfP[6], q = kErfQ[6];
        for (int k = 5; k >= 0; --k) {
            p = p * t + kErfP[k];
            q = q * t + kErfQ[k];
        }
        double e = z * p / q;
        e = z >= kErfLimit ? 1.0 : (z <= -kErfLimit ? -1.0 : e);
        values[i] = static_cast<float>(0.5 * x * (1.0 + e));
    }
}

constexpr Kernels kGeneric{"generic", conv_tile_generic, gelu_generic};

// ---------------------------------------------------------------- 這台電腦用哪一組

#if defined(__x86_64__) || defined(_M_X64)
bool cpu_has_avx2_fma() {
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4] = {};
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool fma = (r[2] & (1 << 12)) != 0, osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
    if (!fma || !osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false;  // 作業系統有沒有在切換執行緒時保存 YMM 暫存器
    __cpuidex(r, 7, 0);
    // AVX2。另外要有 BMI1、BMI2：那個檔案是照 /arch:AVX2 編的，MSVC 在這個選項下也可能用到它們（有 AVX2 的處理器實際上都有）
    return (r[1] & (1 << 5)) != 0 && (r[1] & (1 << 3)) != 0 && (r[1] & (1 << 8)) != 0;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");  // 作業系統不保存 YMM 時也回報沒有
#endif
}
#endif

}  // namespace

const Kernels& generic_kernels() { return kGeneric; }

const Kernels& kernels() {
    static const Kernels* const chosen = [] {
        const char* wanted = std::getenv("NBTV_KERNEL");
        if (wanted != nullptr && std::strcmp(wanted, "generic") == 0) return &kGeneric;
        const Kernels* avx2 = avx2_kernels();
        return avx2 != nullptr ? avx2 : &kGeneric;
    }();
    return *chosen;
}

#if defined(__x86_64__) || defined(_M_X64)
extern const Kernels kAvx2Kernels;  // 在 learned_kernels_avx2.cpp

const Kernels* avx2_kernels() {
    static const bool usable = cpu_has_avx2_fma();
    return usable ? &kAvx2Kernels : nullptr;
}
#else
const Kernels* avx2_kernels() { return nullptr; }
#endif

}  // namespace nbtv::nn
