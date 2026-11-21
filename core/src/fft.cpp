#include "fft.hpp"

#include <cassert>
#include <cmath>
#include <utility>

namespace nbtv {

Fft::Fft(int size) : n_(size), rev_(static_cast<std::size_t>(size)), tw_(static_cast<std::size_t>(size / 2)) {
    assert(size >= 2 && (size & (size - 1)) == 0);
    int bits = 0;
    while ((1 << bits) < n_) ++bits;
    for (int i = 0; i < n_; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        rev_[static_cast<std::size_t>(i)] = r;
    }
    for (int k = 0; k < n_ / 2; ++k) {
        const double a = -kTwoPi * k / n_;
        tw_[static_cast<std::size_t>(k)] = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
}

void Fft::transform(cf* data, bool inverse) const {
    for (int i = 0; i < n_; ++i) {
        const int r = rev_[static_cast<std::size_t>(i)];
        if (r > i) std::swap(data[i], data[r]);
    }
    for (int len = 2; len <= n_; len <<= 1) {
        const int half = len / 2;
        const int step = n_ / len;
        for (int start = 0; start < n_; start += len) {
            for (int k = 0; k < half; ++k) {
                cf w = tw_[static_cast<std::size_t>(k * step)];
                if (inverse) w = std::conj(w);
                const cf a = data[start + k];
                const cf b = data[start + k + half] * w;
                data[start + k] = a + b;
                data[start + k + half] = a - b;
            }
        }
    }
}

}  // namespace nbtv
