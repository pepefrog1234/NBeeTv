// 推論的零件：把計算分給幾條執行緒（parallel.hpp），以及卷積與 GELU 的幾種寫法（learned_kernels.hpp）。
// 整個網路算出來對不對（和 PyTorch 比）在 test_learned.cpp；這裡看的是零件本身。
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "learned_nn.hpp"

using namespace nbtv;
using namespace nbtv::nn;

namespace {

// 固定的亂數（不靠標準函式庫的分布，各平台都一樣）
struct Random {
    std::uint32_t state;
    explicit Random(std::uint32_t seed) : state(seed) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    float unit() { return static_cast<float>(next() & 0xFFFF) / 32768.0f - 1.0f; }  // −1 到 1
};

// ---------------------------------------------------------------- 分給幾條執行緒

// [0, count) 的每一個數剛好被做一次，而且每一段的頭尾都照 grain 切
void check_cover(int count, int grain) {
    std::vector<std::atomic<int>> seen(static_cast<std::size_t>(count));
    std::atomic<int> bad{0};
    parallel_for(count, grain, [&](int begin, int end) {
        if (begin < 0 || end > count || begin >= end || begin % grain != 0 || end != std::min(count, begin + grain)) bad.fetch_add(1);
        for (int i = begin; i < end && i < count; ++i) seen[static_cast<std::size_t>(i)].fetch_add(1);
    });
    int wrong = bad.load();
    for (int i = 0; i < count; ++i) wrong += seen[static_cast<std::size_t>(i)].load() != 1 ? 1 : 0;
    CHECK_EQ(wrong, 0);
}

void test_parallel() {
    const int before = parallel_threads();
    CHECK(before >= 1 && before <= kMaxThreads);
    for (int threads : {1, 2, 3, 8}) {
        set_parallel_threads(threads);
        CHECK_EQ(parallel_threads(), threads);
        check_cover(0, 1);
        check_cover(1, 1);
        check_cover(7, 3);
        check_cover(100, 1);
        check_cover(1000, 7);
        check_cover(4096, 4096);
        check_cover(4097, 4096);
        check_cover(100000, 13);
        // 不合理的 grain 當成 1
        std::atomic<int> total{0};
        parallel_for(50, 0, [&](int begin, int end) { total.fetch_add(end - begin); });
        CHECK_EQ(total.load(), 50);
        parallel_for(-5, 1, [&](int, int) { total.fetch_add(1000); });
        CHECK_EQ(total.load(), 50);
    }

    // 在一段裡面再呼叫：裡面那一次在原地依序做完，結果一樣
    set_parallel_threads(4);
    std::atomic<long long> sum{0};
    parallel_for(64, 1, [&](int begin, int end) {
        for (int i = begin; i < end; ++i)
            parallel_for(10, 3, [&](int first, int last) {
                for (int j = first; j < last; ++j) sum.fetch_add(i * 10 + j);
            });
    });
    CHECK_EQ(sum.load(), 204480);  // Σ(i·10 + j)，i 從 0 到 63、j 從 0 到 9

    // 好幾條執行緒同時各自呼叫（app 裡發射端編碼與接收端解碼會同時發生）
    std::atomic<int> failures{0};
    std::vector<std::thread> callers;
    for (int t = 0; t < 4; ++t)
        callers.emplace_back([&failures, t] {
            for (int round = 0; round < 300; ++round) {
                const int count = 50 + 37 * t + round % 11;
                std::vector<int> seen(static_cast<std::size_t>(count), 0);
                parallel_for(count, 1 + round % 5, [&](int begin, int end) {
                    for (int i = begin; i < end; ++i) ++seen[static_cast<std::size_t>(i)];  // 各段不重疊，不必用 atomic
                });
                for (int v : seen) failures.fetch_add(v != 1 ? 1 : 0);
            }
        });
    for (std::thread& caller : callers) caller.join();
    CHECK_EQ(failures.load(), 0);

    // 很多次很短的工作接連來（幫手才剛睡下又被叫醒）
    long long counted = 0;
    for (int round = 0; round < 20000; ++round) {
        std::atomic<int> part{0};
        parallel_for(16, 1, [&](int begin, int end) { part.fetch_add(end - begin); });
        counted += part.load();
    }
    CHECK_EQ(counted, 320000);

    // 設定的範圍
    set_parallel_threads(100000);
    CHECK_EQ(parallel_threads(), kMaxThreads);
    set_parallel_threads(0);
    CHECK(parallel_threads() >= 1 && parallel_threads() <= kMaxThreads);
    CHECK_EQ(parallel_threads(), before);
}

// ---------------------------------------------------------------- GELU

double gelu_exact(double x) { return 0.5 * x * (1.0 + std::erf(x * 0.70710678118654752440)); }

// 兩個單精度數相差幾個「末位」
long long ulps_apart(float a, float b) {
    std::int32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    const auto key = [](std::int32_t v) { return v < 0 ? static_cast<long long>(std::numeric_limits<std::int32_t>::min()) - v : static_cast<long long>(v); };
    return std::llabs(key(x) - key(y));
}

void test_gelu(const Kernels& kernel) {
    // 有理式本身和 erf 差多少
    double worst_erf = 0.0;
    for (int i = 0; i <= 400000; ++i) {
        const double z = 4.0 * i / 400000.0, t = z * z;
        double p = kErfP[6], q = kErfQ[6];
        for (int k = 5; k >= 0; --k) {
            p = p * t + kErfP[k];
            q = q * t + kErfQ[k];
        }
        worst_erf = std::max(worst_erf, std::fabs(z * p / q - std::erf(z)));
    }
    CHECK(worst_erf < 1e-9);
    CHECK(1.0 - std::erf(kErfLimit) < 1.6e-8);  // 夾到 ±4 之後當成 ±1，差不到這麼多

    // 一整排值：−9 到 9，每 1/4096 一個，再加一些很小、很大的
    std::vector<float> xs;
    for (int i = -9 * 4096; i <= 9 * 4096; ++i) xs.push_back(static_cast<float>(i) / 4096.0f);
    Random random(7);
    for (int i = 0; i < 20000; ++i) xs.push_back(random.unit() * 6.0f);
    for (int e = -40; e <= 30; ++e)
        for (float sign : {1.0f, -1.0f}) xs.push_back(sign * std::ldexp(1.37f, e));
    std::vector<float> ys = xs;
    kernel.gelu(ys.data(), ys.size());

    double worst = 0.0, worst_old = 0.0;
    int out_of_tolerance = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double x = static_cast<double>(xs[i]), want = gelu_exact(x), got = static_cast<double>(ys[i]);
        // 單精度的一個末位，加上「|z| ≥ 4 當成 ±1」的那一點點
        const double tolerance = 1.2e-7 * std::fabs(want) + 1e-8 * std::max(std::fabs(x), 1.0);
        if (!(std::fabs(got - want) <= tolerance)) ++out_of_tolerance;
        worst = std::max(worst, std::fabs(got - want));
        const float old = 0.5f * xs[i] * (1.0f + std::erf(xs[i] * 0.70710678118654752f));  // 原本的寫法
        worst_old = std::max(worst_old, std::fabs(static_cast<double>(old) - want));
        if (std::fabs(x) < 1.0 && x != 0.0) {  // 接近 0 的地方看相對誤差
            if (!(std::fabs(got - want) <= 2.5e-7 * std::fabs(want))) ++out_of_tolerance;
        }
    }
    CHECK_EQ(out_of_tolerance, 0);
    std::printf("  %s 的 GELU：%zu 個值，和精確值最大差 %.2e（原本用 C 函式庫 erf 的寫法是 %.2e）；有理式和 erf 最大差 %.2e\n", kernel.name,
                xs.size(), worst, worst_old, worst_erf);

    // 同一個值不管排在哪裡、一次交幾個，結果都一樣
    int moved = 0;
    for (std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{5}, std::size_t{7}, std::size_t{8}, std::size_t{9}, std::size_t{31}}) {
        for (std::size_t first = 0; first + count <= xs.size(); first += 997) {
            std::vector<float> part(xs.begin() + static_cast<std::ptrdiff_t>(first), xs.begin() + static_cast<std::ptrdiff_t>(first + count));
            kernel.gelu(part.data(), count);
            if (std::memcmp(part.data(), ys.data() + first, count * sizeof(float)) != 0) ++moved;
        }
    }
    CHECK_EQ(moved, 0);

    // 特別的值
    float special[9] = {0.0f, -0.0f, 100.0f, -100.0f, std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), 5.7f};
    kernel.gelu(special, 9);
    CHECK(special[0] == 0.0f && !std::signbit(special[0]));
    CHECK(special[1] == 0.0f && std::signbit(special[1]));
    CHECK(special[2] == 100.0f);
    CHECK(special[3] == 0.0f);  // 很負的輸入是精確的 0
    CHECK(special[4] == std::numeric_limits<float>::max());
    CHECK(special[5] == 0.0f);
    CHECK(std::isinf(special[6]) && special[6] > 0.0f);
    CHECK(std::isnan(special[7]));
    CHECK(special[8] == 5.7f);
    kernel.gelu(special, 0);  // 沒有東西也可以
}

// ---------------------------------------------------------------- 卷積

struct Shape {
    int h, w, cin, cout, k, stride, pad;
};

void fill(const Shape& s, Random& random, Conv& cv, Map& in) {
    cv.cout = s.cout;
    cv.cin = s.cin;
    cv.k = s.k;
    cv.stride = s.stride;
    cv.pad = s.pad;
    cv.w.resize(static_cast<std::size_t>(s.cout) * static_cast<std::size_t>(s.k * s.k * s.cin));
    cv.b.resize(static_cast<std::size_t>(s.cout));
    for (float& v : cv.w) v = random.unit();
    for (float& v : cv.b) v = random.unit();
    in.shape(s.h, s.w, s.cin);
    for (float& v : in.v) v = random.unit() * 2.0f;
    // 一些剛好是 0、−0 的輸入（補零的邊與 GELU 的輸出都會有）
    for (std::size_t i = 0; i < in.v.size(); i += 7) in.v[i] = i % 14 == 0 ? 0.0f : -0.0f;
}

// 照定義直接算（雙精度），以及每個輸出「各項絕對值的和」（用來定容許誤差）
void conv_reference(const Conv& cv, const Map& in, std::vector<double>& out, std::vector<double>& magnitude, int& ho, int& wo) {
    ho = (in.h + 2 * cv.pad - cv.k) / cv.stride + 1;
    wo = (in.w + 2 * cv.pad - cv.k) / cv.stride + 1;
    out.assign(static_cast<std::size_t>(ho) * static_cast<std::size_t>(wo) * static_cast<std::size_t>(cv.cout), 0.0);
    magnitude = out;
    for (int oy = 0; oy < ho; ++oy)
        for (int ox = 0; ox < wo; ++ox)
            for (int oc = 0; oc < cv.cout; ++oc) {
                double sum = cv.b[static_cast<std::size_t>(oc)], mag = std::fabs(sum);
                for (int ky = 0; ky < cv.k; ++ky)
                    for (int kx = 0; kx < cv.k; ++kx) {
                        const int y = oy * cv.stride + ky - cv.pad, x = ox * cv.stride + kx - cv.pad;
                        if (y < 0 || y >= in.h || x < 0 || x >= in.w) continue;
                        const float* p = in.at(y, x);
                        const float* w = cv.w.data() + ((static_cast<std::size_t>(oc) * static_cast<std::size_t>(cv.k) + static_cast<std::size_t>(ky)) *
                                                            static_cast<std::size_t>(cv.k) + static_cast<std::size_t>(kx)) * static_cast<std::size_t>(cv.cin);
                        for (int c = 0; c < cv.cin; ++c) {
                            const double term = static_cast<double>(p[c]) * static_cast<double>(w[c]);
                            sum += term;
                            mag += std::fabs(term);
                        }
                    }
                const std::size_t at = (static_cast<std::size_t>(oy) * static_cast<std::size_t>(wo) + static_cast<std::size_t>(ox)) * static_cast<std::size_t>(cv.cout) +
                                       static_cast<std::size_t>(oc);
                out[at] = sum;
                magnitude[at] = mag;
            }
}

// 照「分 16 路、每一列從第 0 路排起、兩兩相加」的規則用純量算一次；fused 是乘加合成一個運算（std::fma）。
// avx2 的寫法每一個運算都是指定的，結果必須和 fused 的這一份逐位元相同。
std::vector<float> conv_by_lanes(const Conv& cv, const Map& in, bool fused) {
    const int ho = (in.h + 2 * cv.pad - cv.k) / cv.stride + 1, wo = (in.w + 2 * cv.pad - cv.k) / cv.stride + 1;
    const int wp = in.w + 2 * cv.pad, seg = cv.k * cv.cin;
    std::vector<float> padded(static_cast<std::size_t>(in.h + 2 * cv.pad) * static_cast<std::size_t>(wp) * static_cast<std::size_t>(cv.cin), 0.0f);
    for (int y = 0; y < in.h; ++y)
        std::memcpy(padded.data() + (static_cast<std::size_t>(y + cv.pad) * static_cast<std::size_t>(wp) + static_cast<std::size_t>(cv.pad)) * static_cast<std::size_t>(cv.cin),
                    in.at(y, 0), static_cast<std::size_t>(in.w) * static_cast<std::size_t>(cv.cin) * sizeof(float));
    std::vector<float> out(static_cast<std::size_t>(ho) * static_cast<std::size_t>(wo) * static_cast<std::size_t>(cv.cout));
    for (int oy = 0; oy < ho; ++oy)
        for (int ox = 0; ox < wo; ++ox)
            for (int oc = 0; oc < cv.cout; ++oc) {
                float a[16] = {};
                for (int r = 0; r < cv.k; ++r) {
                    const float* x = padded.data() + (static_cast<std::size_t>(oy * cv.stride + r) * static_cast<std::size_t>(wp) + static_cast<std::size_t>(ox * cv.stride)) *
                                                         static_cast<std::size_t>(cv.cin);
                    const float* p = cv.w.data() + (static_cast<std::size_t>(oc) * static_cast<std::size_t>(cv.k) + static_cast<std::size_t>(r)) * static_cast<std::size_t>(seg);
                    for (int i = 0; i < seg; ++i) {
                        if (fused) {
                            a[i % 16] = std::fma(x[i], p[i], a[i % 16]);
                        } else {
                            volatile float product = x[i] * p[i];  // 先存成單精度，編譯器就不能把乘加合起來
                            a[i % 16] += product;
                        }
                    }
                }
                float b[8], c[4];
                for (int j = 0; j < 8; ++j) b[j] = a[j] + a[j + 8];
                for (int j = 0; j < 4; ++j) c[j] = b[j] + b[j + 4];
                out[(static_cast<std::size_t>(oy) * static_cast<std::size_t>(wo) + static_cast<std::size_t>(ox)) * static_cast<std::size_t>(cv.cout) + static_cast<std::size_t>(oc)] =
                    ((c[0] + c[2]) + (c[1] + c[3])) + cv.b[static_cast<std::size_t>(oc)];
            }
    return out;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

void test_conv(const Kernels& kernel) {
    const Shape shapes[] = {
        {7, 9, 3, 32, 5, 1, 2},     // 編碼器的第一層：一列 15 個值，全部是尾巴
        {6, 5, 3, 7, 5, 1, 2},      // 輸出通道不是 4 的倍數
        {9, 12, 28, 64, 1, 1, 0},   // 解碼器的第一層（1×1，一列 28 個）
        {9, 12, 46, 20, 1, 1, 0},   // 一列 46 個：尾巴超過 8 個
        {3, 4, 144, 16, 1, 1, 0},   // 剛好是 16 的倍數的 1×1
        {5, 6, 5, 9, 3, 1, 1},      // 一列 15 個，三列
        {5, 6, 7, 6, 3, 2, 1},      // 一列 21 個，步幅 2
        {4, 4, 10, 4, 1, 1, 0},     // 尾巴 10 個
        {4, 4, 1, 5, 1, 1, 0},      // 一列只有 1 個
        {6, 7, 16, 16, 3, 1, 1},    // 一般的 3×3
        {8, 8, 32, 64, 3, 2, 1},    // 把解析度減半的那一種
        {6, 6, 64, 3, 5, 1, 2},     // 解碼器最後一層：三個輸出通道
        {5, 5, 48, 23, 3, 1, 1},    // 輸出通道 23
        {3, 3, 2, 2, 9, 1, 4},      // 核心的列數比補零的上限多（一般寫法走一個一個算的路）
        {18, 24, 64, 64, 3, 1, 1},  // 夠大，會分給別的執行緒
        {36, 48, 3, 32, 5, 1, 2},   // 同上，而且是有尾巴的
    };
    Random random(20261126);
    int index = 0;
    for (const Shape& s : shapes) {
        Conv cv;
        Map in, out, single;
        fill(s, random, cv, in);
        std::vector<double> want, magnitude;
        int ho = 0, wo = 0;
        conv_reference(cv, in, want, magnitude, ho, wo);

        set_parallel_threads(8);
        conv2d(cv, in, out, kernel);
        CHECK(out.h == ho && out.w == wo && out.c == s.cout && out.v.size() == want.size());
        int off = 0;
        double worst = 0.0;
        for (std::size_t i = 0; i < want.size() && i < out.v.size(); ++i) {
            const double error = std::fabs(static_cast<double>(out.v[i]) - want[i]);
            if (!(error <= 2e-6 * magnitude[i] + 1e-6)) ++off;
            worst = std::max(worst, error / (magnitude[i] + 1e-6));
        }
        CHECK_EQ(off, 0);

        // 只用一條執行緒，結果逐位元相同
        set_parallel_threads(1);
        conv2d(cv, in, single, kernel);
        CHECK(same_bits(out.v, single.v));

        // 和照規則用純量算的比
        const bool is_avx2 = std::strcmp(kernel.name, "avx2") == 0;
        const bool like_fused = same_bits(out.v, conv_by_lanes(cv, in, true));
        const bool like_plain = same_bits(out.v, conv_by_lanes(cv, in, false));
        if (is_avx2) CHECK(like_fused);
        std::printf("  %s 第 %2d 種（%d×%d，%d→%d，%d×%d，步幅 %d）：和照定義算的最大相對差 %.1e；%s\n", kernel.name, ++index, s.w, s.h, s.cin, s.cout, s.k,
                    s.k, s.stride, worst,
                    like_fused && like_plain ? "和照規則算的逐位元相同" : like_fused ? "和「乘加合一」照規則算的逐位元相同" : like_plain ? "和「乘加分開」照規則算的逐位元相同" : "和照規則算的只差在末位");
    }
    set_parallel_threads(0);
}

// 輸入裡有非數值時，只有用到它的輸出受影響。核心的一列不是 16 的倍數時，內層迴圈會多讀幾個值（下一個像素的前幾個通道）
// 再遮掉；沒有遮好的話，隔壁像素的非數值會漏進來——所以非數值要放在每一個通道各試一次。
void test_conv_nan(const Kernels& kernel) {
    Random random(99);
    int wrong = 0, cases = 0;
    // 5×5 的核心、三個輸入通道（一列 15 個值，多讀 1 個）
    for (int channel = 0; channel < 3; ++channel) {
        Conv cv;
        Map in, out;
        fill({6, 8, 3, 8, 5, 1, 2}, random, cv, in);
        in.at(2, 3)[channel] = std::numeric_limits<float>::quiet_NaN();
        conv2d(cv, in, out, kernel);
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x) {
                const bool touched = std::abs(y - 2) <= 2 && std::abs(x - 3) <= 2;  // 5×5 的核心蓋得到 (2, 3) 的輸出
                for (int c = 0; c < out.c; ++c) wrong += std::isnan(out.at(y, x)[c]) != touched ? 1 : 0;
            }
        ++cases;
    }
    // 1×1 的核心、28 個輸入通道（尾巴 12 個，多讀 4 個）；以及 21 個（尾巴 5 個，多讀 11 個）
    for (int cin : {28, 21}) {
        for (int channel : {0, 1, 3, 10, cin - 1}) {
            Conv cv;
            Map in, out;
            fill({4, 5, cin, 8, 1, 1, 0}, random, cv, in);
            in.at(1, 2)[channel] = std::numeric_limits<float>::quiet_NaN();
            conv2d(cv, in, out, kernel);
            for (int y = 0; y < out.h; ++y)
                for (int x = 0; x < out.w; ++x)
                    for (int c = 0; c < out.c; ++c) wrong += std::isnan(out.at(y, x)[c]) != (y == 1 && x == 2) ? 1 : 0;
            ++cases;
        }
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(cases, 13);
}

void test_selection() {
    const Kernels& chosen = kernels();
    const char* wanted = std::getenv("NBTV_KERNEL");
    const bool forced_generic = wanted != nullptr && std::strcmp(wanted, "generic") == 0;
    CHECK(std::strcmp(generic_kernels().name, "generic") == 0);
    if (forced_generic || avx2_kernels() == nullptr) CHECK(&chosen == &generic_kernels());
    else CHECK(&chosen == avx2_kernels());
    if (avx2_kernels() != nullptr) CHECK(std::strcmp(avx2_kernels()->name, "avx2") == 0);
#if !(defined(__x86_64__) || defined(_M_X64))
    CHECK(avx2_kernels() == nullptr);
#endif
    std::printf("  這台電腦上推論用的是 %s，最多 %d 條執行緒%s\n", chosen.name, parallel_threads(),
                avx2_kernels() == nullptr ? "" : forced_generic ? "（avx2 可以用，NBTV_KERNEL 指定不用）" : "");
}

}  // namespace

int main() {
    test_selection();
    test_parallel();
    test_gelu(generic_kernels());
    test_conv(generic_kernels());
    test_conv_nan(generic_kernels());
    if (const Kernels* avx2 = avx2_kernels()) {
        test_gelu(*avx2);
        test_conv(*avx2);
        test_conv_nan(*avx2);
        // 兩種寫法的 GELU 最多差一個末位
        Random random(5);
        std::vector<float> a(50000), b;
        for (float& v : a) v = random.unit() * 7.0f;
        b = a;
        generic_kernels().gelu(a.data(), a.size());
        avx2->gelu(b.data(), b.size());
        long long worst = 0;
        for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, ulps_apart(a[i], b[i]));
        CHECK(worst <= 1);
        std::printf("  generic 與 avx2 的 GELU 最多差 %lld 個末位\n", worst);
    }
    return check::finish("kernels");
}
