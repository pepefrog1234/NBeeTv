// 類神經網路推論裡最花時間的兩個迴圈：卷積的內積與 GELU。各有幾種寫法，啟動後第一次用到時依 CPU 選一種：
//   generic  一般的 C++，靠編譯器自動向量化（arm64 上是 NEON；x86-64 上只有 SSE2）；
//   avx2     x86-64 上用 AVX2 與 FMA 的內建函式寫的（2013 年以後的 Intel、2015 年以後的 AMD 處理器大多有），
//            一次算 8 個數；CPU 或作業系統不支援時不用它。
// 每一種算出來的值只差在浮點數的末位（乘加是不是合成一個運算），遠小於通道的雜訊。
//
// 這個標頭刻意只有純資料的結構與函式的宣告：learned_kernels_avx2.cpp 整個檔案是照 AVX2 編譯的，
// 裡面不能有會和別的檔案共用的行內函式（連結器可能留下照 AVX2 編的那一份，在沒有 AVX2 的電腦上一執行就當掉）。
#pragma once

#include <cstddef>

namespace nbtv::nn {

inline constexpr int kConvSlack = 15;
// 核心的一列不是 16 的倍數時，每一列剩下的那幾個值補零湊成 16 個再算。核心的列數不超過這個數才這樣做（模型裡是 1、3、5），
// 更多列的一個輸出通道一個輸出通道算，結果一樣。
inline constexpr int kConvTailRows = 8;

// 卷積的一塊：算輸出的第 [oy_begin, oy_end) 列裡、第 [oc_begin, oc_end) 個通道。
// 特徵圖都是 [列][行][通道]，通道在最內層；權重是 [輸出通道][核心的列][核心的行][輸入通道]。
struct ConvTile {
    const float* src;       // 補好零的輸入；輸出 (0, 0) 的核心左上角在它的最前面。
                            // seg 不是 16 的倍數時，最後一個用到的值後面還要有 kConvSlack 個讀得到的值（內容不拘）：
                            // 每一列的尾巴是一次讀 16 個再把多讀的遮掉。權重不會被多讀。
    const float* weights;   // 全部輸出通道的權重
    const float* bias;      // 每個輸出通道一個
    float* dst;             // 整張輸出
    std::size_t row_step;   // 輸入往下一列要跳過幾個值（補零後的寬 × 輸入通道數）
    std::size_t px_step;    // 輸出往右一格時，輸入跳過幾個值（步幅 × 輸入通道數）
    int stride;             // 輸出往下一列時，輸入往下幾列
    int k;                  // 核心的列數
    int seg;                // 核心的一列對應輸入裡多長的一段（核心的行數 × 輸入通道數）
    int cout;               // 輸出通道數
    int wo;                 // 輸出的寬
    int oc_begin, oc_end;   // oc_begin 是 4 的倍數
    int oy_begin, oy_end;
};

// GELU 裡誤差函數的有理式近似：erf(z) ≈ z·P(z²)/Q(z²)，|z| ≤ kErfLimit。係數由低次到高次。
inline constexpr double kErfLimit = 4.0;
inline constexpr double kInvSqrt2 = 0.70710678118654752440;
inline constexpr double kErfP[7] = {
    1.1283791746115577,     0.16947359892913183,   0.05146848895894309,   0.003088804308043405,
    0.00031876329283413797, 5.27350268609532e-07, -4.021361380592851e-08,
};
inline constexpr double kErfQ[7] = {
    1.0,                    0.48352551045886394,   0.10678739883598555,   0.013791335041935955,
    0.0010822626250302377,  4.487684294345793e-05, -5.246186051644677e-07,
};

struct Kernels {
    const char* name;
    void (*conv_tile)(const ConvTile& tile);
    // 就地把 count 個值換成 GELU。同一個值不管排在哪裡、count 是多少，算出來都一樣。
    void (*gelu)(float* values, std::size_t count);
};

// 這台電腦上用的那一組。第一次呼叫時決定：x86-64 上 CPU 與作業系統都支援 AVX2 與 FMA 就用 avx2，否則 generic。
// 環境變數 NBTV_KERNEL=generic 可以指定不用 avx2（對照、除錯用）。
const Kernels& kernels();

const Kernels& generic_kernels();
// 這台電腦用不了（或不是 x86-64）時是空指標。
const Kernels* avx2_kernels();

}  // namespace nbtv::nn
