// 2 的次方點數的複數 FFT。數據機只用到 256 點，頻譜顯示用 1024 點，不需要更快的演算法。
#pragma once

#include <vector>

#include "types.hpp"

namespace nbtv {

class Fft {
public:
    explicit Fft(int size);  // size 必須是 2 的次方

    int size() const { return n_; }

    // 原地轉換，都不做正規化：
    //   forward: X[k] = Σ x[n]·exp(-j2πkn/N)
    //   inverse: x[n] = Σ X[k]·exp(+j2πkn/N)
    void forward(cf* data) const { transform(data, false); }
    void inverse(cf* data) const { transform(data, true); }

private:
    void transform(cf* data, bool inverse) const;

    int n_;
    std::vector<int> rev_;  // 位元反轉後的索引
    std::vector<cf> tw_;    // exp(-j2πk/N)，k = 0..N/2-1
};

}  // namespace nbtv
