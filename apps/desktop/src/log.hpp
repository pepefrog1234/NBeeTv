// 開發用的輸出（--selftest、--snapshot、--help）：印到標準輸出；另外可以指定一個檔案，同樣的內容也寫一份進去。
// Windows 上這個程式是視窗程式，從檔案總管或某些殼層啟動時沒有標準輸出可以印，要看結果就靠那個檔案（--report）。
// 這些訊息是給開發的人看的，不翻譯，一律是繁體中文（和 macOS 版一樣）。
#pragma once

#include <cstdio>
#include <string>

namespace nbtvgui::log {

inline std::string& report_path() {
    static std::string path;
    return path;
}

inline void line(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (report_path().empty()) return;
    if (std::FILE* file = std::fopen(report_path().c_str(), "ab")) {
        std::fputs(text.c_str(), file);
        std::fputc('\n', file);
        std::fclose(file);
    }
}

}  // namespace nbtvgui::log
