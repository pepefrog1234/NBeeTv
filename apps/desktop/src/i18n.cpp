#include "i18n.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace nbtvgui {
namespace i18n {

namespace {

const char kSource[] = "zh-Hant";
const char kFallback[] = "en";

using Lookup = std::unordered_map<std::string_view, const char*>;

const Lookup& lookup(const std::string& language) {
    static std::unordered_map<std::string, Lookup> tables;
    const auto found = tables.find(language);
    if (found != tables.end()) return found->second;
    Lookup& table = tables[language];
    for (int t = 0; t < kTableCount; ++t) {
        if (language != kTables[t].language) continue;
        table.reserve(kTables[t].count);
        for (unsigned long i = 0; i < kTables[t].count; ++i) table.emplace(kTables[t].entries[i].key, kTables[t].entries[i].value);
    }
    return table;
}

std::string& current() {
    static std::string language = kSource;
    return language;
}

const char* translation(const std::string& key) {
    if (current() == kSource) return nullptr;
    const Lookup& table = lookup(current());
    const auto found = table.find(std::string_view(key));
    return found == table.end() ? nullptr : found->second;
}

std::string lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

}  // namespace

const std::vector<std::string>& languages() {
    static const std::vector<std::string> list = {"zh-Hant", "zh-Hans", "ja", "es", "en"};
    return list;
}

const char* native_name(const std::string& language) {
    if (language == "zh-Hant") return "繁體中文";
    if (language == "zh-Hans") return "简体中文";
    if (language == "ja") return "日本語";
    if (language == "es") return "Español";
    return "English";
}

const std::string& language() { return current(); }

void use(const std::string& language) {
    bool known = false;
    for (const std::string& candidate : languages()) known = known || candidate == language;
    current() = known ? language : kFallback;
}

std::string language_for(const std::vector<std::string>& preferences) {
    for (const std::string& raw : preferences) {
        // 「zh_TW.UTF-8」「zh-Hant-HK」「es-419」：去掉編碼與修飾，拆成一段一段
        std::string code = lower(raw.substr(0, raw.find_first_of(".@")));
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= code.size()) {
            const std::size_t end = code.find_first_of("-_", start);
            parts.push_back(code.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (parts.empty()) continue;
        const std::string& base = parts[0];
        if (base == "ja" || base == "es" || base == "en") return base;
        if (base == "zh") {
            // 有寫字體的照字體；沒有的話照地區：台灣、香港、澳門是繁體，其餘（中國、新加坡，或只寫 zh）是簡體
            bool traditional = false, decided = false;
            for (std::size_t i = 1; i < parts.size() && !decided; ++i) {
                if (parts[i] == "hant" || parts[i] == "hans") {
                    traditional = parts[i] == "hant";
                    decided = true;
                }
            }
            for (std::size_t i = 1; i < parts.size() && !decided; ++i) {
                if (parts[i] == "tw" || parts[i] == "hk" || parts[i] == "mo") {
                    traditional = true;
                    decided = true;
                }
            }
            return traditional ? "zh-Hant" : "zh-Hans";
        }
    }
    return kFallback;
}

std::string system_language() {
    if (const char* forced = std::getenv("NBTV_LANGUAGE"); forced != nullptr && *forced != '\0') {
        for (const std::string& candidate : languages()) {
            if (candidate == forced) return candidate;
        }
    }
    std::vector<std::string> preferences;
    int count = 0;
    if (SDL_Locale** locales = SDL_GetPreferredLocales(&count)) {
        for (int i = 0; i < count; ++i) {
            if (locales[i] == nullptr || locales[i]->language == nullptr) continue;
            std::string code = locales[i]->language;
            if (locales[i]->country != nullptr && *locales[i]->country != '\0') code += std::string("-") + locales[i]->country;
            preferences.push_back(code);
        }
        SDL_free(locales);
    }
    return language_for(preferences);
}

bool has_translation(const std::string& language, const std::string& key) {
    if (language == kSource) return true;
    const Lookup& table = lookup(language);
    return table.find(std::string_view(key)) != table.end();
}

std::string format(const std::string& pattern, const std::vector<Arg>& args) {
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%' || i + 1 >= pattern.size()) {
            out += pattern[i];
            continue;
        }
        std::size_t j = i + 1;
        if (pattern[j] == '%') {
            out += '%';
            i = j;
            continue;
        }
        // 位置：「%2$@」
        std::size_t position = 0, k = j;
        while (k < pattern.size() && pattern[k] >= '0' && pattern[k] <= '9') position = position * 10 + static_cast<std::size_t>(pattern[k++] - '0');
        if (k < pattern.size() && pattern[k] == '$' && position > 0) j = k + 1;
        else position = 0;
        std::string flags;
        while (j < pattern.size() && (pattern[j] == '.' || pattern[j] == '+' || pattern[j] == '-' || (pattern[j] >= '0' && pattern[j] <= '9'))) flags += pattern[j++];
        while (j < pattern.size() && pattern[j] == 'l') ++j;
        if (j >= pattern.size() || (pattern[j] != '@' && pattern[j] != 'd' && pattern[j] != 'f')) {
            out += pattern[i];  // 不是我們認得的記號：原樣留著
            continue;
        }
        const std::size_t index = position > 0 ? position - 1 : next++;
        const char kind = pattern[j];
        i = j;
        if (index >= args.size()) continue;
        const Arg& arg = args[index];
        char buffer[64];
        if (kind == '@') {
            if (arg.kind == Arg::Kind::text) {
                out += arg.text;
            } else if (arg.kind == Arg::Kind::integer) {
                out += std::to_string(arg.integer);
            } else {
                std::snprintf(buffer, sizeof buffer, "%g", arg.real);
                out += buffer;
            }
        } else if (kind == 'd') {
            const long long value = arg.kind == Arg::Kind::integer ? arg.integer : arg.kind == Arg::Kind::real ? static_cast<long long>(arg.real) : 0;
            std::snprintf(buffer, sizeof buffer, ("%" + flags + "lld").c_str(), value);
            out += buffer;
        } else {
            const double value = arg.kind == Arg::Kind::real ? arg.real : arg.kind == Arg::Kind::integer ? static_cast<double>(arg.integer) : 0.0;
            std::snprintf(buffer, sizeof buffer, ("%" + flags + "f").c_str(), value);
            out += buffer;
        }
    }
    return out;
}

}  // namespace i18n

std::string L(const char* key) {
    const char* translated = i18n::translation(key);
    return translated != nullptr ? translated : key;
}

std::string LF(const char* key, const std::vector<Arg>& args) {
    const char* pattern = i18n::translation(key);
    for (const Arg& arg : args) {
        if (arg.kind != Arg::Kind::integer) continue;
        if (arg.integer == 1) {
            if (const char* singular = i18n::translation(std::string(key) + "#1")) pattern = singular;
        }
        break;
    }
    return i18n::format(pattern != nullptr ? pattern : key, args);
}

}  // namespace nbtvgui
