// 極小的測試工具，不依賴任何外部函式庫。
#pragma once

#include <cmath>
#include <cstdio>

namespace check {

inline int g_checks = 0;
inline int g_failures = 0;

inline void record(bool ok, const char* file, int line, const char* expr) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("%s:%d: 檢查失敗：%s\n", file, line, expr);
    }
}

inline void equal(long long a, long long b, const char* file, int line, const char* expr) {
    ++g_checks;
    if (a != b) {
        ++g_failures;
        std::printf("%s:%d: 檢查失敗：%s（左 %lld，右 %lld）\n", file, line, expr, a, b);
    }
}

inline void near(double a, double b, double tol, const char* file, int line, const char* expr) {
    ++g_checks;
    if (!(std::fabs(a - b) <= tol)) {
        ++g_failures;
        std::printf("%s:%d: 檢查失敗：%s（左 %.9g，右 %.9g，容許 %.3g）\n", file, line, expr, a, b, tol);
    }
}

inline int finish(const char* name) {
    std::printf("%s：%d 項檢查，%d 項失敗\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace check

#define CHECK(cond) ::check::record(static_cast<bool>(cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(a, b) ::check::equal((a), (b), __FILE__, __LINE__, #a " == " #b)
#define CHECK_NEAR(a, b, tol) ::check::near((a), (b), (tol), __FILE__, __LINE__, #a " ~ " #b)
