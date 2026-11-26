#include "canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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

/// 存成影片時印在左上角的那一塊字用的字型（等寬的粗體），和畫布的分開。
FontChain& stamp_chain() {
    static FontChain fonts;
    return fonts;
}

/// fonts 的第 index 個字型；讀不了的回傳 nullptr。呼叫時要握著 fonts.mutex。
LoadedFont* font_at(FontChain& fonts, std::size_t index) {
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
    float advance = 0;   // 排完之後筆走到哪裡（含最後一個字右邊的空白）
    float width() const { return any ? max_x - min_x : 0; }
    float height() const { return any ? max_y - min_y : 0; }
};

/// 用 fonts 裡的字型、size 這個字級排一行。呼叫時要握著 fonts.mutex。
Line layout(FontChain& fonts, const std::vector<char32_t>& text, float size) {
    Line line;
    float pen = 0;
    LoadedFont* previous_font = nullptr;
    int previous_glyph = 0;
    for (const char32_t code : text) {
        LoadedFont* font = nullptr;
        int glyph = 0;
        for (std::size_t i = 0; i < fonts.files.size(); ++i) {
            LoadedFont* candidate = font_at(fonts, i);
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
    line.advance = pen;
    return line;
}

/// 畫布的字型排一行。呼叫時要握著 chain().mutex。
Line layout(const std::vector<char32_t>& text, float size) { return layout(chain(), text, size); }

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
        if (font_at(chain(), i) != nullptr) return true;
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
    for (std::size_t i = 0; i < chain().files.size() && !usable; ++i) usable = font_at(chain(), i) != nullptr;
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

// ---------------------------------------------------------------- 存成影片時印在左上角的字

namespace {

/// 讀寫都要握著 stamp_chain().mutex。
bool& stamp_fonts_chosen() {
    static bool chosen = false;
    return chosen;
}

/// 還沒有人指定字型的話，自己去找系統的等寬粗體字。呼叫時要握著 stamp_chain().mutex。
void ensure_stamp_fonts() {
    if (stamp_fonts_chosen()) return;
    stamp_fonts_chosen() = true;
    stamp_chain().files = find_fonts("en").mono_bold;   // 要印的只有數字與英文字母，照哪一種語言找都一樣
    stamp_chain().loaded.clear();
}

/// 排好的那一塊字：字、它佔的位置、基線在哪裡。呼叫時要握著 stamp_chain().mutex。沒有字型時 usable 是 false。
struct StampLayout {
    bool usable = false;
    Line line;
    VideoStamp::Rect rect;
    int baseline = 0;   // 基線在畫面的第幾列
};

StampLayout layout_stamp(const std::string& text) {
    StampLayout result;
    FontChain& fonts = stamp_chain();
    ensure_stamp_fonts();
    LoadedFont* first = nullptr;
    for (std::size_t i = 0; i < fonts.files.size() && first == nullptr; ++i) first = font_at(fonts, i);
    if (first == nullptr) return result;
    std::vector<char32_t> codes = decode(text);
    // 負號用的是數學的減號（U+2212）；手上的字型都沒有這個字的話，改用一般的連字號，不要印出一個方框
    for (char32_t& code : codes) {
        if (code != 0x2212) continue;
        bool found = false;
        for (std::size_t i = 0; i < fonts.files.size() && !found; ++i) {
            LoadedFont* font = font_at(fonts, i);
            found = font != nullptr && stbtt_FindGlyphIndex(&font->info, static_cast<int>(code)) != 0;
        }
        if (!found) code = U'-';
    }
    if (codes.empty()) return result;
    result.line = layout(fonts, codes, VideoStamp::kFontSize);
    // 方塊的高度照字型的上緣與下緣（不是這幾個字實際有墨的範圍）：不管印的是哪幾個數字，方塊都一樣高
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&first->info, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForMappingEmToPixels(&first->info, VideoStamp::kFontSize);
    const float up = static_cast<float>(ascent) * scale, down = static_cast<float>(-descent) * scale;
    result.rect.x = VideoStamp::kMargin;
    result.rect.y = VideoStamp::kMargin;
    result.rect.width = static_cast<int>(std::ceil(result.line.advance)) + 2 * VideoStamp::kPaddingX;
    result.rect.height = static_cast<int>(std::ceil(up + down)) + 2 * VideoStamp::kPaddingY;
    // 基線放在整數的像素上，橫的筆畫才不會糊成兩列
    result.baseline = result.rect.y + result.rect.height - VideoStamp::kPaddingY - static_cast<int>(std::lround(down));
    result.usable = true;
    return result;
}

}  // namespace

void VideoStamp::set_fonts(const std::vector<FontFile>& fonts) {
    std::lock_guard<std::mutex> lock(stamp_chain().mutex);
    stamp_fonts_chosen() = true;
    stamp_chain().files = fonts;
    stamp_chain().loaded.clear();
}

bool VideoStamp::has_fonts() {
    std::lock_guard<std::mutex> lock(stamp_chain().mutex);
    ensure_stamp_fonts();
    for (std::size_t i = 0; i < stamp_chain().files.size(); ++i) {
        if (font_at(stamp_chain(), i) != nullptr) return true;
    }
    return false;
}

VideoStamp::Rect VideoStamp::rect_for(const std::string& text) {
    std::lock_guard<std::mutex> lock(stamp_chain().mutex);
    const StampLayout stamp = layout_stamp(text);
    return stamp.usable ? stamp.rect : Rect{};
}

bool VideoStamp::draw(const std::string& text, std::uint8_t* rgb, int width, int height) {
    std::lock_guard<std::mutex> lock(stamp_chain().mutex);
    const StampLayout stamp = layout_stamp(text);
    const Rect& box = stamp.rect;
    if (!stamp.usable || rgb == nullptr || box.x + box.width > width || box.y + box.height > height) return false;

    // 1. 半透明的黑底圓角方塊：四個角落照「離圓心多遠」算蓋住多少，邊緣才是平滑的
    for (int y = 0; y < box.height; ++y) {
        for (int x = 0; x < box.width; ++x) {
            // 這個像素的中心到方塊「往內縮一個圓角半徑」之後的那個長方形有多遠
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float dx = std::max({kCornerRadius - px, px - (static_cast<float>(box.width) - kCornerRadius), 0.0f});
            const float dy = std::max({kCornerRadius - py, py - (static_cast<float>(box.height) - kCornerRadius), 0.0f});
            const float cover = std::clamp(kCornerRadius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            if (cover <= 0) continue;
            const float keep = 1.0f - kOpacity * cover;
            std::uint8_t* pixel = rgb + (static_cast<std::size_t>(box.y + y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(box.x + x)) * 3;
            for (int c = 0; c < 3; ++c) pixel[c] = static_cast<std::uint8_t>(std::lround(static_cast<float>(pixel[c]) * keep));
        }
    }

    // 2. 白色的字
    const float origin = static_cast<float>(box.x + kPaddingX);
    std::vector<std::uint8_t> bitmap;
    for (const PlacedGlyph& placed : stamp.line.glyphs) {
        const float pen = origin + placed.x;
        const float shift = pen - std::floor(pen);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&placed.font->info, placed.glyph, placed.scale, placed.scale, shift, 0, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) continue;
        bitmap.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
        stbtt_MakeGlyphBitmapSubpixel(&placed.font->info, bitmap.data(), w, h, w, placed.scale, placed.scale, shift, 0, placed.glyph);
        const int left = static_cast<int>(std::floor(pen)) + x0, upper = stamp.baseline + y0;
        for (int y = 0; y < h; ++y) {
            const int py = upper + y;
            if (py < 0 || py >= height) continue;
            for (int x = 0; x < w; ++x) {
                const int px = left + x;
                if (px < 0 || px >= width) continue;
                const int ink = bitmap[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
                if (ink == 0) continue;
                std::uint8_t* pixel = rgb + (static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)) * 3;
                for (int c = 0; c < 3; ++c) pixel[c] = static_cast<std::uint8_t>(pixel[c] + ((255 - pixel[c]) * ink + 127) / 255);
            }
        }
    }
    return true;
}

namespace sequence_video {

std::vector<std::uint8_t> frame_image(const Picture& picture, const std::string& label) {
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(kWidth) * kHeight * 3);
    // 一個像素變成 kScale×kScale 的一格：先做好一列，再原樣抄 kScale 次
    const std::size_t row_bytes = static_cast<std::size_t>(kWidth) * 3;
    for (int y = 0; y < kImageHeight; ++y) {
        std::uint8_t* row = frame.data() + static_cast<std::size_t>(y) * kScale * row_bytes;
        const std::uint8_t* source = picture.rgb.data() + static_cast<std::size_t>(y) * kImageWidth * 3;
        for (int x = 0; x < kImageWidth; ++x) {
            for (int k = 0; k < kScale; ++k) std::memcpy(row + (static_cast<std::size_t>(x) * kScale + static_cast<std::size_t>(k)) * 3, source + static_cast<std::size_t>(x) * 3, 3);
        }
        for (int k = 1; k < kScale; ++k) std::memcpy(row + static_cast<std::size_t>(k) * row_bytes, row, row_bytes);
    }
    if (!label.empty()) VideoStamp::draw(label, frame.data(), kWidth, kHeight);
    return frame;
}

}  // namespace sequence_video

}  // namespace nbtvgui
