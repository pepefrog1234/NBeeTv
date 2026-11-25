#include "canvas.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace nbtvgui {

namespace {

constexpr float kSideMargin = 3;
constexpr float kPadding = 3;     // 字帶上下的留白
constexpr float kLineGap = 2;
constexpr float kBandOpacity = 0.62f;

struct LoadedFont {
    const FontData* data = nullptr;   // 字型檔對應到記憶體的那一塊（fonts.hpp），一直都在
    stbtt_fontinfo info{};
    bool ok = false;
};

struct FontChain {
    std::mutex mutex;
    std::vector<FontFile> files;
    std::vector<std::unique_ptr<LoadedFont>> loaded;   // 用到才讀進來（中日文的字型檔很大）
};

FontChain& chain() {
    static FontChain fonts;
    return fonts;
}

/// 第 index 個字型；讀不了的回傳 nullptr。呼叫時要握著 chain().mutex。
LoadedFont* font_at(std::size_t index) {
    FontChain& fonts = chain();
    if (index >= fonts.files.size()) return nullptr;
    if (fonts.loaded.size() <= index) fonts.loaded.resize(fonts.files.size());
    if (!fonts.loaded[index]) {
        auto font = std::make_unique<LoadedFont>();
        font->data = font_data(fonts.files[index].path);
        if (font->data != nullptr) {
            const int offset = stbtt_GetFontOffsetForIndex(font->data->bytes, fonts.files[index].index);
            font->ok = offset >= 0 && stbtt_InitFont(&font->info, font->data->bytes, offset) != 0;
        }
        fonts.loaded[index] = std::move(font);
    }
    return fonts.loaded[index]->ok ? fonts.loaded[index].get() : nullptr;
}

std::vector<char32_t> decode(const std::string& text) {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        if (i + static_cast<std::size_t>(length) > text.size()) length = 1;
        char32_t code = length == 1 ? c : c & (0xFF >> (length + 1));
        for (int k = 1; k < length; ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]) & 0x3F);
        out.push_back(code);
        i += static_cast<std::size_t>(length);
    }
    return out;
}

std::string encode(const std::vector<char32_t>& codes) {
    std::string out;
    for (const char32_t code : codes) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return out;
}

struct PlacedGlyph {
    LoadedFont* font = nullptr;
    int glyph = 0;
    float scale = 0;
    float x = 0;   // 筆的位置（基線上，相對於這一行的開頭）
};

/// 排好的一行：每個字放在哪裡，以及實際有墨的範圍（相對於基線，y 往下是正的）。
struct Line {
    std::vector<PlacedGlyph> glyphs;
    float min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool any = false;
    float width() const { return any ? max_x - min_x : 0; }
    float height() const { return any ? max_y - min_y : 0; }
};

/// 用 size 這個字級排一行。呼叫時要握著 chain().mutex。
Line layout(const std::vector<char32_t>& text, float size) {
    Line line;
    float pen = 0;
    LoadedFont* previous_font = nullptr;
    int previous_glyph = 0;
    for (const char32_t code : text) {
        LoadedFont* font = nullptr;
        int glyph = 0;
        for (std::size_t i = 0; i < chain().files.size(); ++i) {
            LoadedFont* candidate = font_at(i);
            if (candidate == nullptr) continue;
            const int found = stbtt_FindGlyphIndex(&candidate->info, static_cast<int>(code));
            if (found != 0) {
                font = candidate;
                glyph = found;
                break;
            }
            if (font == nullptr) font = candidate;   // 每個字型都沒有這個字：用第一個字型的「缺字」
        }
        if (font == nullptr) continue;
        const float scale = stbtt_ScaleForMappingEmToPixels(&font->info, size);
        if (previous_font == font && previous_glyph != 0) pen += scale * static_cast<float>(stbtt_GetGlyphKernAdvance(&font->info, previous_glyph, glyph));
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&font->info, glyph, &advance, &bearing);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        const float shift = pen - std::floor(pen);
        stbtt_GetGlyphBitmapBoxSubpixel(&font->info, glyph, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
        if (x1 > x0 && y1 > y0) {
            const float left = std::floor(pen) + static_cast<float>(x0), right = std::floor(pen) + static_cast<float>(x1);
            line.min_x = line.any ? std::min(line.min_x, left) : left;
            line.max_x = line.any ? std::max(line.max_x, right) : right;
            line.min_y = line.any ? std::min(line.min_y, static_cast<float>(y0)) : static_cast<float>(y0);
            line.max_y = line.any ? std::max(line.max_y, static_cast<float>(y1)) : static_cast<float>(y1);
            line.any = true;
        }
        line.glyphs.push_back({font, glyph, scale, pen});
        pen += scale * static_cast<float>(advance);
        previous_font = font;
        previous_glyph = glyph;
    }
    return line;
}

/// 每一行都放得進 width 的最大字級（從 maximum 起每次縮半點）；用最小的字還放不下時回傳 0。
float fit(const std::vector<std::vector<char32_t>>& lines, float width, float maximum) {
    for (float size = maximum; size >= CanvasLayer::kMinimumFontSize; size -= 0.5f) {
        float widest = 0;
        for (const auto& text : lines) widest = std::max(widest, layout(text, size).width());
        if (widest <= width) return size;
    }
    return 0;
}

/// 在某個空白處拆成兩行，讓比較長的那一行愈短愈好。沒有空白可以拆時回傳空的。
std::vector<std::vector<char32_t>> split_in_two(const std::vector<char32_t>& text) {
    std::vector<std::vector<char32_t>> words(1);
    for (const char32_t code : text) {
        if (code == U' ') {
            if (!words.back().empty()) words.emplace_back();
        } else {
            words.back().push_back(code);
        }
    }
    if (words.back().empty()) words.pop_back();
    if (words.size() < 2) return {};
    const auto join = [&](std::size_t from, std::size_t to) {
        std::vector<char32_t> joined;
        for (std::size_t i = from; i < to; ++i) {
            if (i > from) joined.push_back(U' ');
            joined.insert(joined.end(), words[i].begin(), words[i].end());
        }
        return joined;
    };
    std::vector<std::vector<char32_t>> best;
    std::size_t best_length = static_cast<std::size_t>(-1);
    for (std::size_t cut = 1; cut < words.size(); ++cut) {
        const auto first = join(0, cut), second = join(cut, words.size());
        const std::size_t longer = std::max(first.size(), second.size());
        if (longer < best_length) {
            best_length = longer;
            best = {first, second};
        }
    }
    return best;
}

std::vector<char32_t> trimmed(std::vector<char32_t> text) {
    const auto blank = [](char32_t c) { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0x3000; };
    while (!text.empty() && blank(text.back())) text.pop_back();
    std::size_t first = 0;
    while (first < text.size() && blank(text[first])) ++first;
    text.erase(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(first));
    return text;
}

}  // namespace

float font_line_height_per_em(const FontData& data, int index) {
    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(data.bytes, index);
    if (offset < 0 || stbtt_InitFont(&info, data.bytes, offset) == 0) return 1.0f;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    const float ratio = static_cast<float>(ascent - descent) * stbtt_ScaleForMappingEmToPixels(&info, 1.0f);
    return ratio > 0.5f && ratio < 3.0f ? ratio : 1.0f;
}

void CanvasLayer::set_fonts(const std::vector<FontFile>& fonts) {
    std::lock_guard<std::mutex> lock(chain().mutex);
    chain().files = fonts;
    chain().loaded.clear();
}

bool CanvasLayer::has_fonts() {
    std::lock_guard<std::mutex> lock(chain().mutex);
    for (std::size_t i = 0; i < chain().files.size(); ++i) {
        if (font_at(i) != nullptr) return true;
    }
    return false;
}

std::shared_ptr<const CanvasLayer> CanvasLayer::make(const CanvasStyle& style) {
    std::vector<char32_t> text = trimmed(decode(style.text));
    if (text.size() > static_cast<std::size_t>(kMaximumCharacters)) text.resize(static_cast<std::size_t>(kMaximumCharacters));
    text = trimmed(text);
    if (text.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(chain().mutex);
    bool usable = false;
    for (std::size_t i = 0; i < chain().files.size() && !usable; ++i) usable = font_at(i) != nullptr;
    if (!usable) return nullptr;

    const float available = static_cast<float>(kImageWidth) - 2 * kSideMargin;

    // 1. 決定排成幾行、字多大
    std::vector<std::vector<char32_t>> chosen = {text};
    float size = fit(chosen, available, kMaximumFontSize);
    if (size < kWrapBelowFontSize) {
        const auto split = split_in_two(text);
        if (!split.empty()) {
            const float two_line_size = fit(split, available, kWrappedMaximumFontSize);
            if (two_line_size > size) {
                chosen = split;
                size = two_line_size;
            }
        }
    }
    const float font_size = size > 0 ? size : kMinimumFontSize;
    // 用最小的字還是放不下：截短，結尾補「…」
    if (size <= 0) {
        for (auto& line : chosen) {
            if (layout(line, font_size).width() <= available) continue;
            while (!line.empty()) {
                line.pop_back();
                while (!line.empty() && line.back() == U' ') line.pop_back();
                std::vector<char32_t> candidate = line;
                candidate.push_back(0x2026);
                if (layout(candidate, font_size).width() <= available || line.empty()) {
                    line = candidate;
                    break;
                }
            }
        }
    }

    // 2. 每一行實際有墨的範圍：字帶只留這麼高，不浪費畫面
    std::vector<Line> typeset;
    float ink_height = 0;
    for (const auto& line : chosen) {
        typeset.push_back(layout(line, font_size));
        ink_height += std::ceil(typeset.back().height());
    }
    ink_height += static_cast<float>(typeset.size() - 1) * kLineGap;
    const int band = static_cast<int>(std::min(static_cast<float>(kImageHeight), std::ceil(ink_height + 2 * kPadding)));

    // 3. 畫字（每個像素被字蓋住多少）
    auto layer = std::shared_ptr<CanvasLayer>(new CanvasLayer());
    layer->style_ = style;
    for (const auto& line : chosen) layer->lines_.push_back(encode(line));
    layer->font_size_ = font_size;
    layer->first_row_ = style.position == CanvasPosition::top ? 0 : kImageHeight - band;
    layer->last_row_ = layer->first_row_ + band;
    layer->coverage_.assign(static_cast<std::size_t>(kImageWidth) * kImageHeight, 0);

    float top = static_cast<float>(layer->first_row_) + (static_cast<float>(band) - ink_height) / 2;   // 第一行有墨範圍的上緣
    std::vector<std::uint8_t> bitmap;
    for (const Line& line : typeset) {
        // 基線放在整數的像素上，橫的筆畫才不會糊成兩列
        const int baseline = static_cast<int>(std::lround(top - line.min_y));
        const float origin = std::round((static_cast<float>(kImageWidth) - line.width()) / 2 - line.min_x);
        for (const PlacedGlyph& placed : line.glyphs) {
            const float pen = origin + placed.x;
            const float shift = pen - std::floor(pen);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBoxSubpixel(&placed.font->info, placed.glyph, placed.scale, placed.scale, shift, 0, &x0, &y0, &x1, &y1);
            const int w = x1 - x0, h = y1 - y0;
            if (w <= 0 || h <= 0) continue;
            bitmap.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
            stbtt_MakeGlyphBitmapSubpixel(&placed.font->info, bitmap.data(), w, h, w, placed.scale, placed.scale, shift, 0, placed.glyph);
            const int left = static_cast<int>(std::floor(pen)) + x0, upper = baseline + y0;
            for (int y = 0; y < h; ++y) {
                const int py = upper + y;
                if (py < layer->first_row_ || py >= layer->last_row_) continue;
                for (int x = 0; x < w; ++x) {
                    const int px = left + x;
                    if (px < 0 || px >= kImageWidth) continue;
                    std::uint8_t& cell = layer->coverage_[static_cast<std::size_t>(py) * kImageWidth + static_cast<std::size_t>(px)];
                    cell = static_cast<std::uint8_t>(std::min(255, cell + bitmap[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]));
                }
            }
        }
        top += std::ceil(line.height()) + kLineGap;
    }
    return layer;
}

Picture CanvasLayer::apply(const Picture& picture) const {
    Picture out = picture;
    for (int y = first_row_; y < last_row_; ++y) {
        for (int x = 0; x < kImageWidth; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * kImageWidth + static_cast<std::size_t>(x);
            // 這一層是「半透明的黑帶上面疊白字」：字蓋住 c 的地方，白色佔 c、底下的畫面只剩 (1 − c)·(1 − 0.62)
            const float c = static_cast<float>(coverage_[index]) / 255.0f;
            const int alpha = static_cast<int>(std::lround(255.0f * (c + (1 - c) * kBandOpacity)));
            const int white = static_cast<int>(std::lround(255.0f * c));
            for (int channel = 0; channel < 3; ++channel) {
                const int under = (static_cast<int>(out.rgb[3 * index + static_cast<std::size_t>(channel)]) * (255 - alpha) + 127) / 255;
                out.rgb[3 * index + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(std::min(255, under + white));
            }
        }
    }
    return out;
}

}  // namespace nbtvgui
