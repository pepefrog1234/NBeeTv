// 命令列工具共用：每個工具在 main() 開頭呼叫一次 tool_startup()。
//   - 找出執行檔所在的目錄，權重檔預設就從它旁邊的 models/ 找（model_file.hpp），
//     所以編好的執行檔連同 models/ 搬到別的目錄、別台電腦都能直接用；
//   - Windows：主控台的輸出改成 UTF-8，說明與訊息裡的中文才不會變成亂碼（結束時改回原來的）。
// 這個檔案會引入各平台自己的標頭（Windows 是 <windows.h>），所以只給工具的 main 用；測試不引入它。
#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <limits.h>
#include <cstdint>
#include <cstdlib>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "model_file.hpp"

namespace nbtv::tools {

// 這個執行檔的完整路徑；查不出來時是空字串（那就只找環境變數與建置時專案的 models/）。
inline std::string executable_path() {
#if defined(_WIN32)
    // 用 A 結尾的版本：拿到的編碼和 fopen 吃的一樣（執行檔的 manifest 指定了 UTF-8，見 tools/windows/utf8.manifest）
    std::string path(32768, '\0');
    const DWORD n = GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) return {};
    path.resize(n);
    return path;
#elif defined(__APPLE__)
    std::string raw(1024, '\0');
    std::uint32_t size = static_cast<std::uint32_t>(raw.size());
    if (_NSGetExecutablePath(raw.data(), &size) != 0) {  // 不夠大：size 被改成需要的大小
        raw.assign(size, '\0');
        if (_NSGetExecutablePath(raw.data(), &size) != 0) return {};
    }
    // 經由符號連結執行時（例如連結放在 /usr/local/bin），要找的是真正那一份旁邊的 models/
    char resolved[PATH_MAX];
    return realpath(raw.c_str(), resolved) != nullptr ? std::string(resolved) : std::string(raw.c_str());
#elif defined(__linux__)
    std::string path(4096, '\0');
    const ssize_t n = readlink("/proc/self/exe", path.data(), path.size());  // 已經是解開符號連結之後的路徑
    if (n <= 0 || static_cast<std::size_t>(n) >= path.size()) return {};
    path.resize(static_cast<std::size_t>(n));
    return path;
#else
    return {};
#endif
}

// 路徑裡最後一個分隔符號之前的部分；沒有分隔符號時是空字串。
inline std::string directory_of(const std::string& path) {
#if defined(_WIN32)
    const std::size_t slash = path.find_last_of("/\\");
#else
    const std::size_t slash = path.rfind('/');
#endif
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

#if defined(_WIN32)
namespace detail {
inline unsigned int& original_console_cp() {
    static unsigned int cp = 0;
    return cp;
}
}  // namespace detail
#endif

inline void tool_startup() {
#if defined(_WIN32)
    // 主控台的字碼頁是那個視窗的設定，不是這個行程的：改了要在結束時改回去，
    // 不然同一個視窗裡之後執行的別的程式會跟著變。沒有主控台時（GetConsoleOutputCP 回傳 0）不必動。
    const UINT cp = GetConsoleOutputCP();
    if (cp != 0 && cp != CP_UTF8 && SetConsoleOutputCP(CP_UTF8) != 0) {
        detail::original_console_cp() = cp;
        std::atexit([] { SetConsoleOutputCP(detail::original_console_cp()); });
    }
#endif
    program_dir() = directory_of(executable_path());
}

}  // namespace nbtv::tools
