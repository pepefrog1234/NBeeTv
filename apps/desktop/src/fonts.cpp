#include "fonts.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>

#include "process.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nbtvgui {

namespace {

bool exists(const std::string& path) {
    SDL_PathInfo info{};
    return !path.empty() && SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

/// 把存在的檔案加進清單（同一個檔不重複）。回傳有沒有加進去。
bool add(std::vector<FontFile>& list, const std::string& path, int index = 0) {
    if (!exists(path)) return false;
    for (const FontFile& known : list) {
        if (known.path == path && known.index == index) return false;
    }
    list.push_back({path, index});
    return true;
}

#if !defined(_WIN32) && !defined(__APPLE__)
/// 問 fontconfig：這個樣式會用哪一個字型檔。沒有 fc-match 或查不到時回傳空的。
FontFile fc_match(const std::string& fc_match_program, const std::string& pattern) {
    if (fc_match_program.empty()) return {};
    const std::optional<std::string> output = run_and_capture({fc_match_program, "--format=%{file}\n%{index}\n", pattern}, 3);
    if (!output) return {};
    const std::size_t newline = output->find('\n');
    if (newline == std::string::npos) return {};
    FontFile file;
    file.path = output->substr(0, newline);
    file.index = std::atoi(output->c_str() + newline + 1);
    return file;
}
#endif

}  // namespace

FontSet find_fonts(const std::string& language) {
    FontSet fonts;
    // 中文、日文的字型照介面的語言排優先順序；其他語言時繁體在前
    const bool hans = language == "zh-Hans", japanese = language == "ja";

#if defined(_WIN32)
    const char* root = std::getenv("WINDIR");
    if (root == nullptr || *root == '\0') root = std::getenv("SystemRoot");
    const std::string dir = std::string(root != nullptr && *root != '\0' ? root : "C:\\Windows") + "\\Fonts\\";
    add(fonts.regular, dir + "segoeui.ttf");
    add(fonts.bold, dir + "seguisb.ttf") || add(fonts.bold, dir + "segoeuib.ttf");
    add(fonts.mono, dir + "consola.ttf") || add(fonts.mono, dir + "cour.ttf");
    add(fonts.mono_bold, dir + "consolab.ttf") || add(fonts.mono_bold, dir + "courbd.ttf");
    struct Cjk {
        const char* regular;
        const char* bold;
    };
    const Cjk traditional{"msjh.ttc", "msjhbd.ttc"}, simplified{"msyh.ttc", "msyhbd.ttc"}, japan{"YuGothR.ttc", "YuGothB.ttc"}, japan_old{"meiryo.ttc", "meiryob.ttc"};
    std::vector<Cjk> order = japanese ? std::vector<Cjk>{japan, japan_old, traditional, simplified}
                             : hans ? std::vector<Cjk>{simplified, traditional, japan, japan_old}
                                    : std::vector<Cjk>{traditional, simplified, japan, japan_old};
    for (const Cjk& cjk : order) {
        const bool found = add(fonts.regular, dir + cjk.regular);
        fonts.has_cjk = fonts.has_cjk || found;
        const bool bold_found = add(fonts.bold, dir + cjk.bold);
        if (!bold_found && found) add(fonts.bold, dir + cjk.regular);
        if (found) add(fonts.mono, dir + cjk.regular);
        if (bold_found) add(fonts.mono_bold, dir + cjk.bold);
        else if (found) add(fonts.mono_bold, dir + cjk.regular);
    }
#elif defined(__APPLE__)
    // 開發時在 macOS 上試用的：發佈給 macOS 的是另一個 app
    (void)hans;
    add(fonts.regular, "/System/Library/Fonts/Supplemental/Arial.ttf") || add(fonts.regular, "/System/Library/Fonts/Helvetica.ttc");
    add(fonts.bold, "/System/Library/Fonts/Supplemental/Arial Bold.ttf") || add(fonts.bold, "/System/Library/Fonts/Helvetica.ttc", 1);
    add(fonts.mono, "/System/Library/Fonts/Menlo.ttc") || add(fonts.mono, "/System/Library/Fonts/Monaco.ttf");
    add(fonts.mono_bold, "/System/Library/Fonts/Menlo.ttc", 1);
    // 中文、日文：每一項是「一般、粗體」兩個字型（檔案與 .ttc 裡的第幾個）。日文介面時日文的字型排前面
    struct Cjk {
        const char* regular;
        int regular_index;
        const char* bold;
        int bold_index;
    };
    const Cjk chinese{"/System/Library/Fonts/Hiragino Sans GB.ttc", 0, "/System/Library/Fonts/Hiragino Sans GB.ttc", 2};
    const Cjk japan{"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", 0, "/System/Library/Fonts/ヒラギノ角ゴシック W6.ttc", 0};
    const Cjk heiti{"/System/Library/Fonts/STHeiti Light.ttc", 0, "/System/Library/Fonts/STHeiti Medium.ttc", 0};
    for (const Cjk& cjk : japanese ? std::vector<Cjk>{japan, chinese, heiti} : std::vector<Cjk>{chinese, heiti, japan}) {
        const bool found = add(fonts.regular, cjk.regular, cjk.regular_index);
        fonts.has_cjk = fonts.has_cjk || found;
        if (!found) continue;
        add(fonts.mono, cjk.regular, cjk.regular_index);
        if (!add(fonts.bold, cjk.bold, cjk.bold_index)) add(fonts.bold, cjk.regular, cjk.regular_index);
        if (!add(fonts.mono_bold, cjk.bold, cjk.bold_index)) add(fonts.mono_bold, cjk.regular, cjk.regular_index);
    }
#else
    const std::string fc = find_program("fc-match", {"/usr/bin", "/usr/local/bin"});
    const std::string cjk_tag = japanese ? "ja" : hans ? "zh-cn" : "zh-tw";
    const FontFile regular = fc_match(fc, "sans-serif"), bold = fc_match(fc, "sans-serif:bold"), mono = fc_match(fc, "monospace"), mono_bold = fc_match(fc, "monospace:bold");
    add(fonts.regular, regular.path, regular.index);
    add(fonts.bold, bold.path, bold.index);
    add(fonts.mono, mono.path, mono.index);
    add(fonts.mono_bold, mono_bold.path, mono_bold.index);
    // 沒有 fontconfig（或它什麼都沒給）時試幾個常見的位置
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"}) {
        if (!fonts.regular.empty()) break;
        add(fonts.regular, path);
    }
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"}) {
        if (!fonts.bold.empty()) break;
        add(fonts.bold, path);
    }
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
                             "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"}) {
        if (!fonts.mono.empty()) break;
        add(fonts.mono, path);
    }
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf",
                             "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf"}) {
        if (!fonts.mono_bold.empty()) break;
        add(fonts.mono_bold, path);
    }
    if (fonts.mono_bold.empty()) fonts.mono_bold = fonts.mono;
    // 中文、日文：先問 fontconfig 這個語言用哪一個，再補上另外兩種語言的（別人的呼號旁邊可能有別種文字的裝置名稱）
    std::vector<std::string> tags = {cjk_tag};
    for (const char* tag : {"zh-tw", "zh-cn", "ja"}) {
        if (tag != cjk_tag) tags.push_back(tag);
    }
    for (const std::string& tag : tags) {
        const FontFile cjk = fc_match(fc, "sans-serif:lang=" + tag), cjk_bold = fc_match(fc, "sans-serif:bold:lang=" + tag);
        // fontconfig 找不到那個語言的字型時會給一個不相干的；檔名看起來像中日文字型的才算
        const auto looks_cjk = [](const std::string& path) {
            for (const char* word : {"CJK", "cjk", "Han", "han", "WenQuanYi", "wqy", "uming", "ukai", "Droid", "ipa", "IPA", "VL-", "Takao", "Source"}) {
                if (path.find(word) != std::string::npos) return true;
            }
            return false;
        };
        if (!looks_cjk(cjk.path)) continue;
        const bool found = add(fonts.regular, cjk.path, cjk.index);
        fonts.has_cjk = fonts.has_cjk || found || exists(cjk.path);
        const bool bold_found = looks_cjk(cjk_bold.path) && exists(cjk_bold.path);
        add(fonts.bold, bold_found ? cjk_bold.path : cjk.path, bold_found ? cjk_bold.index : cjk.index);
        add(fonts.mono, cjk.path, cjk.index);
        add(fonts.mono_bold, bold_found ? cjk_bold.path : cjk.path, bold_found ? cjk_bold.index : cjk.index);
    }
    if (!fonts.has_cjk) {
        // Noto Sans CJK 的 .ttc 裡依序是日文、韓文、簡體、繁體、香港
        const int index = japanese ? 0 : hans ? 2 : 3;
        for (const char* path : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"}) {
            if (!add(fonts.regular, path, index)) continue;
            add(fonts.bold, path, index);
            add(fonts.mono, path, index);
            add(fonts.mono_bold, path, index);
            fonts.has_cjk = true;
            break;
        }
    }
#endif
    if (fonts.bold.empty()) fonts.bold = fonts.regular;
    if (fonts.mono.empty()) fonts.mono = fonts.regular;
    if (fonts.mono_bold.empty()) fonts.mono_bold = fonts.mono;
    return fonts;
}

const FontData* font_data(const std::string& path) {
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<FontData>> mapped;   // 開不了的也記著（值是空的），不必每次再試
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto found = mapped.find(path); found != mapped.end()) return found->second ? found->second.get() : nullptr;
    std::unique_ptr<FontData> data;
#if defined(_WIN32)
    // 執行檔的 manifest 指定了 UTF-8，A 結尾的函式吃的就是 UTF-8 的路徑
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
            if (const HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
                if (const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) {
                    data = std::make_unique<FontData>();
                    data->bytes = static_cast<const unsigned char*>(view);
                    data->size = static_cast<std::size_t>(size.QuadPart);
                }
                CloseHandle(mapping);   // 對應出來的那一塊還在，一直留到程式結束
            }
        }
        CloseHandle(file);
    }
#else
    const int file = open(path.c_str(), O_RDONLY);
    if (file >= 0) {
        struct stat info {};
        if (fstat(file, &info) == 0 && info.st_size > 0) {
            void* view = mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, file, 0);
            if (view != MAP_FAILED) {
                data = std::make_unique<FontData>();
                data->bytes = static_cast<const unsigned char*>(view);
                data->size = static_cast<std::size_t>(info.st_size);
            }
        }
        close(file);   // 對應出來的那一塊還在，一直留到程式結束
    }
#endif
    const FontData* result = data.get();
    mapped[path] = std::move(data);
    return result;
}

}  // namespace nbtvgui
