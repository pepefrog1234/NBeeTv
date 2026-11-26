// 卷積的內積與 GELU 在 x86-64 上用 AVX2 與 FMA 的寫法（learned_kernels.hpp）。別的處理器上這個檔案是空的。
//
// 分路、補零、求和的順序都和 learned_kernels.cpp 的一般寫法相同，差別只有兩個：一次算 8 個數，
// 乘加用的是合成一個運算的 FMA。只有確認過 CPU 與作業系統都支援之後才會被呼叫（learned_kernels.cpp 的 avx2_kernels）。
//
// 注意：這個檔案在 MSVC 上整個照 /arch:AVX2 編譯（core/CMakeLists.txt），所以裡面不可以引用會產生行內函式的標頭
// （<vector>、<algorithm>、<cmath> 這些），也不可以定義別的檔案會用到的行內函式——連結器同名的只留一份，
// 留到這裡編的那一份的話，沒有 AVX2 的電腦一執行到就當掉。GCC 與 Clang 不靠編譯選項，只有標了 NBTV_AVX2 的函式照 AVX2 編。
#include "learned_kernels.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

#include <cstddef>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#define NBTV_AVX2 __attribute__((target("avx2,fma")))
#else
#define NBTV_AVX2
#endif

namespace nbtv::nn {

namespace {

// 前 8 個全是 1、後 8 個是 0：從第 8 − count 個讀起，就是「前 count 路有效」的遮罩
alignas(32) const std::int32_t kMaskTable[16] = {-1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0};

NBTV_AVX2 inline __m256i lane_mask(int count) {
    return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kMaskTable + 8 - count));
}

// 十六路加成一個數（lo 是第 0–7 路，hi 是第 8–15 路）：兩兩相加四次，順序和一般寫法的 sum_lanes 相同
NBTV_AVX2 inline float sum16(__m256 lo, __m256 hi) {
    const __m256 b = _mm256_add_ps(lo, hi);                                                // b[j] = a[j] + a[j + 8]
    const __m128 c = _mm_add_ps(_mm256_castps256_ps128(b), _mm256_extractf128_ps(b, 1));  // c[j] = b[j] + b[j + 4]
    const __m128 d = _mm_add_ps(c, _mm_movehl_ps(c, c));                                   // d[0] = c[0] + c[2]，d[1] = c[1] + c[3]
    return _mm_cvtss_f32(_mm_add_ss(d, _mm_shuffle_ps(d, d, 1)));                          // d[0] + d[1]
}

// 一個輸出像素、四個輸出通道；核心的一列是 16 的倍數
NBTV_AVX2 inline void pixel_dot4(const float* in, std::size_t row_step, const float* w0, const float* w1, const float* w2,
                                 const float* w3, int rows, int n, float out[4]) {
    __m256 a0l = _mm256_setzero_ps(), a0h = a0l, a1l = a0l, a1h = a0l, a2l = a0l, a2h = a0l, a3l = a0l, a3h = a0l;
    for (int r = 0; r < rows; ++r) {
        const float* x = in + static_cast<std::size_t>(r) * row_step;
        const std::size_t off = static_cast<std::size_t>(r) * static_cast<std::size_t>(n);
        const float* p0 = w0 + off;
        const float* p1 = w1 + off;
        const float* p2 = w2 + off;
        const float* p3 = w3 + off;
        for (int i = 0; i + 16 <= n; i += 16) {
            const __m256 xl = _mm256_loadu_ps(x + i), xh = _mm256_loadu_ps(x + i + 8);
            a0l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p0 + i), a0l);
            a0h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p0 + i + 8), a0h);
            a1l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p1 + i), a1l);
            a1h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p1 + i + 8), a1h);
            a2l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p2 + i), a2l);
            a2h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p2 + i + 8), a2h);
            a3l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p3 + i), a3l);
            a3h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p3 + i + 8), a3h);
        }
    }
    out[0] = sum16(a0l, a0h);
    out[1] = sum16(a1l, a1h);
    out[2] = sum16(a2l, a2h);
    out[3] = sum16(a3l, a3h);
}

// 四組權重每一列的尾巴，補零湊成 16 個（和一般寫法的 Tails4 一樣）
struct Tails4 {
    float w[4][kConvTailRows][16];
};

// 同上，一列不是 16 的倍數：尾巴那幾個值湊成 16 個來算，多出來的那幾路是 0，加上去累加器不變。
// 輸入這邊直接讀 16 個再把多讀的遮成 0（ConvTile 保證讀得到）；權重讀的是補好零的那一份（tails）。
// 不用「遮罩載入」去讀界外的位置：Intel 保證遮掉的位置不會出錯，AMD 的手冊說由各處理器自己決定。
NBTV_AVX2 inline void pixel_dot4_ragged(const float* in, std::size_t row_step, const float* w0, const float* w1, const float* w2,
                                        const float* w3, const Tails4& tails, int rows, int n, float out[4]) {
    const int rem = n % 16, full = n - rem;
    const __m256 keep_l = _mm256_castsi256_ps(lane_mask(rem < 8 ? rem : 8)), keep_h = _mm256_castsi256_ps(lane_mask(rem > 8 ? rem - 8 : 0));
    __m256 a0l = _mm256_setzero_ps(), a0h = a0l, a1l = a0l, a1h = a0l, a2l = a0l, a2h = a0l, a3l = a0l, a3h = a0l;
    for (int r = 0; r < rows; ++r) {
        const float* x = in + static_cast<std::size_t>(r) * row_step;
        const std::size_t off = static_cast<std::size_t>(r) * static_cast<std::size_t>(n);
        const float* p0 = w0 + off;
        const float* p1 = w1 + off;
        const float* p2 = w2 + off;
        const float* p3 = w3 + off;
        for (int i = 0; i < full; i += 16) {
            const __m256 xl = _mm256_loadu_ps(x + i), xh = _mm256_loadu_ps(x + i + 8);
            a0l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p0 + i), a0l);
            a0h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p0 + i + 8), a0h);
            a1l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p1 + i), a1l);
            a1h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p1 + i + 8), a1h);
            a2l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p2 + i), a2l);
            a2h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p2 + i + 8), a2h);
            a3l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(p3 + i), a3l);
            a3h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(p3 + i + 8), a3h);
        }
        const __m256 xl = _mm256_and_ps(_mm256_loadu_ps(x + full), keep_l), xh = _mm256_and_ps(_mm256_loadu_ps(x + full + 8), keep_h);
        a0l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(tails.w[0][r]), a0l);
        a0h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(tails.w[0][r] + 8), a0h);
        a1l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(tails.w[1][r]), a1l);
        a1h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(tails.w[1][r] + 8), a1h);
        a2l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(tails.w[2][r]), a2l);
        a2h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(tails.w[2][r] + 8), a2h);
        a3l = _mm256_fmadd_ps(xl, _mm256_loadu_ps(tails.w[3][r]), a3l);
        a3h = _mm256_fmadd_ps(xh, _mm256_loadu_ps(tails.w[3][r] + 8), a3h);
    }
    out[0] = sum16(a0l, a0h);
    out[1] = sum16(a1l, a1h);
    out[2] = sum16(a2l, a2h);
    out[3] = sum16(a3l, a3h);
}

// 一個輸出通道（輸出通道數不是 4 的倍數時剩下的，以及核心的列數太多、沒辦法整組補零的）。
// 一列不是 16 的倍數時，權重的尾巴每一列抄一份補零的再算（這條路走得少，不講究快）。
NBTV_AVX2 inline float pixel_dot1(const float* in, std::size_t row_step, const float* w, int rows, int n) {
    const int rem = n % 16, full = n - rem;
    const __m256 keep_l = _mm256_castsi256_ps(lane_mask(rem < 8 ? rem : 8)), keep_h = _mm256_castsi256_ps(lane_mask(rem > 8 ? rem - 8 : 0));
    __m256 al = _mm256_setzero_ps(), ah = al;
    for (int r = 0; r < rows; ++r) {
        const float* x = in + static_cast<std::size_t>(r) * row_step;
        const float* p = w + static_cast<std::size_t>(r) * static_cast<std::size_t>(n);
        for (int i = 0; i < full; i += 16) {
            al = _mm256_fmadd_ps(_mm256_loadu_ps(x + i), _mm256_loadu_ps(p + i), al);
            ah = _mm256_fmadd_ps(_mm256_loadu_ps(x + i + 8), _mm256_loadu_ps(p + i + 8), ah);
        }
        if (rem != 0) {
            float tail[16] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            for (int j = 0; j < rem; ++j) tail[j] = p[full + j];
            al = _mm256_fmadd_ps(_mm256_and_ps(_mm256_loadu_ps(x + full), keep_l), _mm256_loadu_ps(tail), al);
            ah = _mm256_fmadd_ps(_mm256_and_ps(_mm256_loadu_ps(x + full + 8), keep_h), _mm256_loadu_ps(tail + 8), ah);
        }
    }
    return sum16(al, ah);
}

NBTV_AVX2 void conv_tile_avx2(const ConvTile& t) {
    const std::size_t per_out = static_cast<std::size_t>(t.k) * static_cast<std::size_t>(t.seg);  // 一個輸出通道的權重個數
    const std::size_t out_c = static_cast<std::size_t>(t.cout);
    const std::size_t out_row = static_cast<std::size_t>(t.wo) * out_c;
    const std::size_t in_row = static_cast<std::size_t>(t.stride) * t.row_step;
    const int rem = t.seg % 16, full = t.seg - rem;

    // 和一般寫法同樣的三條路，各寫一份迴圈，最內層才單純
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
            for (int c = 0; c < 4; ++c) {
                w[c] = t.weights + static_cast<std::size_t>(oc + c) * per_out;
                for (int r = 0; r < t.k; ++r) {
                    const float* from = w[c] + static_cast<std::size_t>(r) * static_cast<std::size_t>(t.seg) + static_cast<std::size_t>(full);
                    for (int j = 0; j < 16; ++j) tails.w[c][r][j] = j < rem ? from[j] : 0.0f;
                }
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
    _mm256_zeroupper();
}

// 四個值的 GELU：式子、係數、運算的順序都和 learned_kernels.cpp 的 gelu_generic 相同（那邊有說明）
NBTV_AVX2 inline __m128 gelu4(__m128 values) {
    const __m256d limit = _mm256_set1_pd(kErfLimit), minus_limit = _mm256_set1_pd(-kErfLimit), one = _mm256_set1_pd(1.0);
    const __m256d x = _mm256_cvtps_pd(values);
    // 夾到 ±4。max、min 在其中一邊是非數值時回傳第二個運算元，z 放在第二個，非數值才會照樣傳下去
    const __m256d z = _mm256_min_pd(limit, _mm256_max_pd(minus_limit, _mm256_mul_pd(x, _mm256_set1_pd(kInvSqrt2))));
    const __m256d t = _mm256_mul_pd(z, z);
    __m256d p = _mm256_set1_pd(kErfP[6]), q = _mm256_set1_pd(kErfQ[6]);
    for (int k = 5; k >= 0; --k) {
        p = _mm256_fmadd_pd(p, t, _mm256_set1_pd(kErfP[k]));
        q = _mm256_fmadd_pd(q, t, _mm256_set1_pd(kErfQ[k]));
    }
    __m256d e = _mm256_div_pd(_mm256_mul_pd(z, p), q);
    e = _mm256_blendv_pd(e, one, _mm256_cmp_pd(z, limit, _CMP_GE_OQ));
    e = _mm256_blendv_pd(e, _mm256_set1_pd(-1.0), _mm256_cmp_pd(z, minus_limit, _CMP_LE_OQ));
    return _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_mul_pd(_mm256_set1_pd(0.5), x), _mm256_add_pd(one, e)));
}

NBTV_AVX2 void gelu_avx2(float* values, std::size_t count) {
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) _mm_storeu_ps(values + i, gelu4(_mm_loadu_ps(values + i)));
    if (i < count) {
        // 不滿四個的也湊成四個來算：每個值走的都是同一段程式，排在哪裡結果都一樣
        float last[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (std::size_t j = i; j < count; ++j) last[j - i] = values[j];
        _mm_storeu_ps(last, gelu4(_mm_loadu_ps(last)));
        for (std::size_t j = i; j < count; ++j) values[j] = last[j - i];
    }
    _mm256_zeroupper();
}

}  // namespace

// 常數初始化的資料，沒有要執行的程式；learned_kernels.cpp 確認 CPU 支援之後才把它交出去
extern const Kernels kAvx2Kernels;
const Kernels kAvx2Kernels = {"avx2", conv_tile_avx2, gelu_avx2};

}  // namespace nbtv::nn

#endif
