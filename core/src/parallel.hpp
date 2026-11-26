// 把一件可以切開的計算分給幾條執行緒一起做。核心裡只有類神經網路的推論用到（learned_nn.hpp 的卷積與 GELU）：
// 一張畫面的編碼、解碼原本只用一條執行緒，慢的電腦上一幀的時間裡算不完。
//
// 幫手的執行緒是整個行程共用的一組，第一次用到時才開，之後留著等下一件工作，行程結束時跟著消失
// （不在結束時收回：app 結束的那一刻別的執行緒可能還在推論）。同時有好幾條執行緒各自呼叫 parallel_for 也可以，
// 幫手會輪流去做。
//
// 結果和只用一條執行緒時逐位元相同：切開的每一段各算各的，誰先做、在哪條執行緒上做都不影響算出來的值。
#pragma once

#include <type_traits>

namespace nbtv {

namespace detail {
void parallel_run(int count, int grain, void (*fn)(void*, int, int), void* context);
}

// 把 [0, count) 切成每段 grain 個（最後一段可能比較短），對每一段呼叫 fn(begin, end)；全部做完才返回。
// 呼叫的這條執行緒自己也做，另外最多用 parallel_threads() − 1 條幫手。
// fn 會同時在幾條執行緒上執行：各段之間不可以互相依賴，不可以丟出例外，也不要在裡面再呼叫 parallel_for
// （再呼叫的那一次會在原地依序做完）。只有一段、或設定成只用一條執行緒時，就在這條執行緒上依序做。
template <typename Fn>
void parallel_for(int count, int grain, Fn&& fn) {
    using Body = std::remove_reference_t<Fn>;
    detail::parallel_run(
        count, grain, [](void* context, int begin, int end) { (*static_cast<Body*>(context))(begin, end); },
        const_cast<void*>(static_cast<const void*>(&fn)));
}

// 推論最多同時用幾條執行緒（含呼叫的那一條）。
// 預設是這台電腦的邏輯處理器數，最多 kDefaultMaxThreads；環境變數 NBTV_THREADS 有設的話用它（1 就是完全不開執行緒）。
int parallel_threads();
// 改成 threads 條（1 到 kMaxThreads；超出範圍的夾進來）；0 回到預設。隨時可以改，下一次 parallel_for 起生效。
void set_parallel_threads(int threads);

inline constexpr int kDefaultMaxThreads = 8;
inline constexpr int kMaxThreads = 64;

}  // namespace nbtv
