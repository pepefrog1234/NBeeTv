// 畫布：疊在送出畫面上的一層字（例如「CQ BX4ACP」），發射前印在每一張畫面上。排法照 macOS 版的 Canvas.swift：
// 字盡量大（最大 18），放不下才縮，縮到 12 以下還放不下就在空白處拆成兩行，再放不下的結尾截成「…」。
// 字帶的樣子只跟文字與位置有關，所以先算好一張 96×72 的圖層，之後每一張畫面疊上去就好。
// 字型是 fonts.hpp 找到的系統粗體字（用 stb_truetype 畫），所以同一句字在不同的電腦上筆畫會有一點不同。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core_bridge.hpp"
#include "fonts.hpp"

namespace nbtvgui {

/// 一個字型的行高（上緣到下緣）是字身的幾倍：Arial 約 1.12，Noto Sans CJK 約 1.45。讀不了回傳 1。
/// 介面用它把 Dear ImGui 的字級（指的是行高）換成字身的大小，不同的字型畫出來才會一樣大。
float font_line_height_per_em(const FontData& data, int index);

enum class CanvasPosition { top, bottom };

/// 畫布的內容：印什麼字、印在畫面的上緣還是下緣。
struct CanvasStyle {
    std::string text;
    CanvasPosition position = CanvasPosition::top;
    bool operator==(const CanvasStyle& other) const { return text == other.text && position == other.position; }
};

/// 算好的圖層。建立之後不會再改，可以在任何執行緒上使用。
class CanvasLayer {
public:
    static constexpr float kMaximumFontSize = 18;
    static constexpr float kMinimumFontSize = 9;
    /// 一行放得下、但字得縮到比這個還小的時候，改排成兩行
    static constexpr float kWrapBelowFontSize = 12;
    /// 排成兩行時字最大到這裡：再大的話，字帶會蓋掉快一半的畫面
    static constexpr float kWrappedMaximumFontSize = 12;
    static constexpr int kMaximumCharacters = 40;

    /// 畫布用的字型（依序找字）。程式開始時設定一次。
    static void set_fonts(const std::vector<FontFile>& fonts);
    static bool has_fonts();

    /// 沒有字可以印（或沒有字型）時回傳空的。
    static std::shared_ptr<const CanvasLayer> make(const CanvasStyle& style);

    /// 疊到一張畫面上，回傳疊好的畫面。字帶以外的地方一個位元組都不動。
    Picture apply(const Picture& picture) const;

    const CanvasStyle& style() const { return style_; }
    /// 實際排出來的幾行字（太長時拆成兩行；再放不下的那一行結尾是「…」）
    const std::vector<std::string>& lines() const { return lines_; }
    float font_size() const { return font_size_; }
    /// 字帶佔畫面的哪幾列（從上往下數，含 first、不含 last）
    int first_row() const { return first_row_; }
    int last_row() const { return last_row_; }

private:
    CanvasLayer() = default;
    CanvasStyle style_;
    std::vector<std::string> lines_;
    float font_size_ = 0;
    int first_row_ = 0, last_row_ = 0;
    std::vector<std::uint8_t> coverage_;   // 96×72：每個像素被字蓋住多少（0–255）
};

}  // namespace nbtvgui
