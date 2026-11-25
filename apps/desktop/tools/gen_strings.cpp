// 建置時用的小工具：把介面的翻譯表（.strings，和 macOS 版同一種格式）變成一個 C++ 原始檔，編進執行檔裡；
// 同時檢查程式裡用 L("…")、LF("…") 寫的每一句在每個語言都有翻譯、要填的值對得上，少了就讓建置失敗。
//
// 用法：gen_strings --out 輸出.cpp --strings 目錄 [--strings 目錄 …] --sources 檔案…
// 每個 --strings 的目錄底下是 <語言>.lproj/Localizable.strings；後面的目錄蓋過前面的（桌面版自己的蓋過 macOS 版的）。
// 原始碼裡的文字是繁體中文，同時也是索引，所以繁體中文不需要翻譯表。
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* const kLanguages[] = {"zh-Hans", "ja", "es", "en"};

bool read_text(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

// 從 text[pos]（一個雙引號）開始讀一個帶跳脫的字串；pos 移到結尾的引號之後。
bool read_quoted(const std::string& text, std::size_t& pos, std::string& out) {
    if (pos >= text.size() || text[pos] != '"') return false;
    out.clear();
    for (++pos; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (c == '"') {
            ++pos;
            return true;
        }
        if (c == '\\' && pos + 1 < text.size()) {
            const char next = text[++pos];
            out += next == 'n' ? '\n' : next == 't' ? '\t' : next == 'r' ? '\r' : next;
        } else {
            out += c;
        }
    }
    return false;
}

void skip_space_and_comments(const std::string& text, std::size_t& pos) {
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++pos;
        } else if (text.compare(pos, 2, "//") == 0) {
            while (pos < text.size() && text[pos] != '\n') ++pos;
        } else if (text.compare(pos, 2, "/*") == 0) {
            const std::size_t end = text.find("*/", pos + 2);
            pos = end == std::string::npos ? text.size() : end + 2;
        } else {
            break;
        }
    }
}

// 「"原文" = "翻譯";」一行一句。讀不懂的地方回報在哪裡。
bool parse_strings(const std::string& path, std::map<std::string, std::string>& table) {
    std::string text;
    if (!read_text(path, text)) return true;  // 沒有這個檔案：這個目錄沒有替這個語言準備東西
    std::size_t pos = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;
    for (;;) {
        skip_space_and_comments(text, pos);
        if (pos >= text.size()) return true;
        std::string key, value;
        bool ok = read_quoted(text, pos, key);
        skip_space_and_comments(text, pos);
        ok = ok && pos < text.size() && text[pos] == '=';
        if (ok) ++pos;
        skip_space_and_comments(text, pos);
        ok = ok && read_quoted(text, pos, value);
        skip_space_and_comments(text, pos);
        ok = ok && pos < text.size() && text[pos] == ';';
        if (!ok) {
            const long line = 1 + static_cast<long>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(std::min(pos, text.size())), '\n'));
            std::fprintf(stderr, "%s:%ld：讀不懂這一行（要寫成 \"原文\" = \"翻譯\";）\n", path.c_str(), line);
            return false;
        }
        ++pos;
        table[key] = value;
    }
}

// 句子裡要填的值，依序是哪幾種：s（%@）、d（%d、%lld）、f（%.2f 這類）。%1$@ 這種指定位置的照位置排。
std::string placeholders(const std::string& text) {
    std::map<int, char> at;
    int next = 1;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != '%') continue;
        std::size_t j = i + 1;
        if (text[j] == '%') {
            i = j;
            continue;
        }
        int position = 0;
        std::size_t k = j;
        while (k < text.size() && text[k] >= '0' && text[k] <= '9') position = position * 10 + (text[k++] - '0');
        if (k < text.size() && text[k] == '$' && position > 0) j = k + 1;
        else position = 0;
        while (j < text.size() && (text[j] == '.' || text[j] == '+' || text[j] == '-' || (text[j] >= '0' && text[j] <= '9') || text[j] == 'l')) ++j;
        if (j >= text.size()) break;
        const char kind = text[j] == '@' ? 's' : text[j] == 'd' ? 'd' : text[j] == 'f' ? 'f' : 0;
        if (kind == 0) continue;
        at[position > 0 ? position : next++] = kind;
        i = j;
    }
    std::string kinds;
    for (const auto& [position, kind] : at) kinds += kind;
    return kinds;
}

// 原始碼裡 L("…") 與 LF("…" 的第一個參數。
void collect_keys(const std::string& path, std::set<std::string>& keys) {
    std::string text;
    if (!read_text(path, text)) {
        std::fprintf(stderr, "讀不到 %s\n", path.c_str());
        return;
    }
    for (std::size_t pos = 0; pos + 3 < text.size(); ++pos) {
        if (text[pos] != 'L') continue;
        if (pos > 0) {
            const char before = text[pos - 1];
            if (before == '_' || (before >= '0' && before <= '9') || (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z')) continue;
        }
        std::size_t open = pos + 1;
        if (text[open] == 'F') ++open;
        if (text.compare(open, 2, "(\"") != 0) continue;
        std::size_t quote = open + 1;
        std::string key;
        if (read_quoted(text, quote, key) && !key.empty()) keys.insert(key);
    }
}

std::string literal(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path;
    std::vector<std::string> string_dirs, sources;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "--strings" && i + 1 < argc) {
            string_dirs.push_back(argv[++i]);
        } else if (a == "--sources") {
            while (i + 1 < argc) sources.push_back(argv[++i]);
        }
    }
    if (out_path.empty() || string_dirs.empty()) {
        std::fprintf(stderr, "用法：gen_strings --out 輸出.cpp --strings 目錄 [--strings 目錄 …] --sources 檔案…\n");
        return 2;
    }

    std::set<std::string> used;
    for (const std::string& source : sources) collect_keys(source, used);

    bool ok = true;
    std::string code = "// 自動產生的檔案（apps/desktop/tools/gen_strings.cpp），不要直接改：改 Localization 裡的 .strings。\n"
                       "#include \"i18n.hpp\"\n\nnamespace nbtvgui::i18n {\n\n";
    for (const char* language : kLanguages) {
        std::map<std::string, std::string> table;
        for (const std::string& dir : string_dirs) {
            ok = parse_strings(dir + "/" + language + ".lproj/Localizable.strings", table) && ok;
        }
        for (const std::string& key : used) {
            const auto found = table.find(key);
            if (found == table.end()) {
                std::fprintf(stderr, "%s 少了翻譯：%s\n", language, key.c_str());
                ok = false;
            } else if (placeholders(found->second) != placeholders(key)) {
                std::fprintf(stderr, "%s 要填的值和原文對不上：%s → %s\n", language, key.c_str(), found->second.c_str());
                ok = false;
            }
        }
        std::string name = language;
        std::replace(name.begin(), name.end(), '-', '_');
        code += "static const Entry kEntries_" + name + "[] = {\n";
        for (const auto& [key, value] : table) code += "    {" + literal(key) + ", " + literal(value) + "},\n";
        code += "};\n\n";
    }
    code += "const Table kTables[] = {\n";
    for (const char* language : kLanguages) {
        std::string name = language;
        std::replace(name.begin(), name.end(), '-', '_');
        code += std::string("    {\"") + language + "\", kEntries_" + name + ", sizeof kEntries_" + name + " / sizeof kEntries_" + name + "[0]},\n";
    }
    code += "};\nconst int kTableCount = sizeof kTables / sizeof kTables[0];\n\n}  // namespace nbtvgui::i18n\n";
    if (!ok) return 1;

    // 內容沒變就不重寫，免得每次建置都重新編譯用到它的檔案
    std::string existing;
    if (read_text(out_path, existing) && existing == code) return 0;
    std::ofstream out(out_path, std::ios::binary);
    out << code;
    return out ? 0 : 1;
}
