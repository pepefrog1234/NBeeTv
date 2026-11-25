// 介面的語言。和 macOS 版一樣：原始碼裡給使用者看的文字是繁體中文（同時是每一句的索引），
// 其他四種語言的翻譯表在建置時由 gen_strings 編進執行檔（大部分直接沿用 macOS 版的 Localizable.strings，
// 桌面版自己多出來或說法不同的句子在 apps/desktop/Localization）。
#pragma once

#include <string>
#include <vector>

namespace nbtvgui {

/// LF 要填進句子裡的一個值：字串（%@）、整數（%d）或小數（%.1f）。
struct Arg {
    enum class Kind { text, integer, real };
    Kind kind = Kind::text;
    std::string text;
    long long integer = 0;
    double real = 0;

    Arg(const char* value) : kind(Kind::text), text(value != nullptr ? value : "") {}
    Arg(const std::string& value) : kind(Kind::text), text(value) {}
    Arg(int value) : kind(Kind::integer), integer(value) {}
    Arg(long value) : kind(Kind::integer), integer(value) {}
    Arg(long long value) : kind(Kind::integer), integer(value) {}
    Arg(unsigned value) : kind(Kind::integer), integer(value) {}
    Arg(unsigned long value) : kind(Kind::integer), integer(static_cast<long long>(value)) {}
    Arg(unsigned long long value) : kind(Kind::integer), integer(static_cast<long long>(value)) {}
    Arg(double value) : kind(Kind::real), real(value) {}
    Arg(float value) : kind(Kind::real), real(value) {}
};

/// 介面上的一句話，照目前的語言。key 是繁體中文的原文。
std::string L(const char* key);

/// 有要填的值的句子：原文裡用 %@（字串）、%d（整數）、%.1f 這類記號，值依序放在 args；翻譯可以用 %1$@、%2$d 調換順序。
/// 第一個整數是 1、而且翻譯表裡另外有「原文#1」這一項時用那一項（英文、西班牙文的單數）。
std::string LF(const char* key, const std::vector<Arg>& args);

namespace i18n {

struct Entry {
    const char* key;
    const char* value;
};
struct Table {
    const char* language;
    const Entry* entries;
    unsigned long count;
};
extern const Table kTables[];
extern const int kTableCount;

/// 繁體中文、簡體中文、日文、西班牙文、美式英文
const std::vector<std::string>& languages();
/// 每種語言自己的名稱（選單上用）：「繁體中文」「简体中文」「日本語」「Español」「English」
const char* native_name(const std::string& language);

/// 目前用的語言（上面那五個代碼之一）
const std::string& language();
void use(const std::string& language);

/// 一份語言偏好的清單（「zh-TW」「zh_Hant_HK」「es-419」這種代碼，愈前面愈優先）會挑到哪一種語言：
/// 清單裡排最前面、而且有翻譯的那一個；都沒有就是英文。
std::string language_for(const std::vector<std::string>& preferences);

/// 系統的語言偏好挑到的語言。環境變數 NBTV_LANGUAGE 可以直接指定。
std::string system_language();

/// 這一句在某個語言有沒有翻譯（檢查用）
bool has_translation(const std::string& language, const std::string& key);

/// 把值填進句子（%@、%d、%lld、%.Nf、%1$@ 這些記號）。
std::string format(const std::string& pattern, const std::vector<Arg>& args);

}  // namespace i18n
}  // namespace nbtvgui
