// 小型稠密線性方程式：設計 Wiener 內插係數時用，矩陣最大 49×49。
#pragma once

#include <cmath>
#include <complex>  // IWYU pragma: keep（T 是複數時需要 std::abs 的多載）
#include <utility>
#include <vector>

namespace nbtv {

// 解 A·X = B。a 是 n×n（列優先），b 是 n×m（列優先），解寫回 b。
// 使用部分選主元的高斯消去；矩陣奇異時回傳 false。
template <typename T>
bool solve_linear(std::vector<T>& a, std::vector<T>& b, int n, int m) {
    auto A = [&](int r, int c) -> T& { return a[static_cast<std::size_t>(r * n + c)]; };
    auto B = [&](int r, int c) -> T& { return b[static_cast<std::size_t>(r * m + c)]; };
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        double best = std::abs(A(col, col));
        for (int r = col + 1; r < n; ++r) {
            const double v = std::abs(A(r, col));
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best < 1e-14) return false;
        if (pivot != col) {
            for (int c = 0; c < n; ++c) std::swap(A(col, c), A(pivot, c));
            for (int c = 0; c < m; ++c) std::swap(B(col, c), B(pivot, c));
        }
        const T inv = T(1) / A(col, col);
        for (int r = col + 1; r < n; ++r) {
            const T f = A(r, col) * inv;
            if (std::abs(f) == 0.0) continue;
            for (int c = col; c < n; ++c) A(r, c) -= f * A(col, c);
            for (int c = 0; c < m; ++c) B(r, c) -= f * B(col, c);
        }
    }
    for (int col = n - 1; col >= 0; --col) {
        const T inv = T(1) / A(col, col);
        for (int c = 0; c < m; ++c) {
            T acc = B(col, c);
            for (int k = col + 1; k < n; ++k) acc -= A(col, k) * B(k, c);
            B(col, c) = acc * inv;
        }
    }
    return true;
}

}  // namespace nbtv
