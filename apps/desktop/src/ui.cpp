#include "ui.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "i18n.hpp"
#include "imgui.h"

namespace nbtvgui {

namespace {

constexpr ImU32 rgb(unsigned hex, unsigned alpha = 255) { return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, alpha); }

// 顏色照 macOS 版的 Theme.swift（版面稿的數值）
constexpr ImU32 kBackground = rgb(0x12151A);    // 視窗底色
constexpr ImU32 kText = rgb(0xE8ECF0);          // 主要文字
constexpr ImU32 kSecondary = rgb(0xADB6BF);     // 次要文字、標籤
constexpr ImU32 kTertiary = rgb(0x909AA5);      // 說明、停用
constexpr ImU32 kBar = rgb(0x1A1E24);           // 頂列與底列
constexpr ImU32 kBarBorder = rgb(0x0A0C0F);
constexpr ImU32 kPanel = rgb(0x1B2026);         // 面板
constexpr ImU32 kWell = rgb(0x0C0E11);          // 畫面區底色、分段選擇的軌道、輸入框
constexpr ImU32 kSeparator = rgb(0x2A3038);
constexpr ImU32 kButton = rgb(0x252B33);
constexpr ImU32 kButtonHover = rgb(0x2D343D);
constexpr ImU32 kButtonRing = rgb(0x3A424C);
constexpr ImU32 kDisabledFill = rgb(0x1E2329);
constexpr ImU32 kTrack = rgb(0x2B323B);         // 進度條與音量條的軌道
constexpr ImU32 kAside = rgb(0x161A1F);         // 右側欄
constexpr ImU32 kRx = rgb(0x44D6D6);            // 接收：青色
constexpr ImU32 kRxPill = rgb(0x14282B);
constexpr ImU32 kRxPillRing = rgb(0x1F6B6E);
constexpr ImU32 kTx = rgb(0xF9A870);            // 發射：橘色
constexpr ImU32 kTxInk = rgb(0x1D1206);         // 橘色底上的深色字
constexpr ImU32 kWaterfall = rgb(0x05070A);
constexpr ImU32 kPlaceholder = rgb(0x07090C);
constexpr ImU32 kMenu = rgb(0x20262D);
constexpr ImU32 kMenuSelected = rgb(0x2B323B);

ImTextureRef tex(SDL_Texture* texture) { return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture))); }

/// 把使用者按到的事（開檔、存檔的對話框選好了）轉回主執行緒：SDL 的對話框可能在別的執行緒上回報。
/// done_many 有給的話是可以選很多個的對話框：選到的全部交給它；不然只把第一個交給 done。
struct DialogRequest {
    std::function<void(const std::string&)> done;
    std::function<void(const std::vector<std::string>&)> done_many;
};

void SDLCALL dialog_callback(void* userdata, const char* const* files, int) {
    std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
    if (files == nullptr || files[0] == nullptr) return;   // 出錯或按了取消
    if (request->done_many) {
        std::vector<std::string> paths;
        for (const char* const* file = files; *file != nullptr; ++file) paths.emplace_back(*file);
        on_main([done = request->done_many, paths] { done(paths); });
        return;
    }
    const std::string path = files[0];
    on_main([done = request->done, path] { done(path); });
}

std::string file_name(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

UI::UI(App& app, SDL_Renderer* renderer, SDL_Window* window) : app_(app), renderer_(renderer), window_(window) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // 不留 imgui.ini
    io.LogFilename = nullptr;
    apply_style();
    canvas_buffer_ = app_.settings.canvas_text;
}

/// Dear ImGui 自己的元件（選單的項目、輸入框、捲軸）用的間距與顏色。每次都從預設值算起：放大倍率改了也不會愈乘愈大。
void UI::apply_style() {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(scale_);
    style.WindowBorderSize = 0;
    style.WindowPadding = ImVec2(0, 0);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;
    style.FontSizeBase = 13.0f * scale_;
    ImGui::GetStyle() = style;
}

UI::~UI() {
    for (auto& [picture, cached] : textures_) SDL_DestroyTexture(cached.texture);
    if (waterfall_texture_ != nullptr) SDL_DestroyTexture(waterfall_texture_);
}

void UI::set_scale(float scale) {
    scale_ = std::max(0.5f, scale);
    apply_style();
}

/// 在一個跳出來的選單裡呼叫：按了 Esc 就把它關掉。Dear ImGui 要開鍵盤導覽才會自己處理 Esc，這個 app 沒有開。
void UI::close_popup_on_escape() {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
}

void UI::note_hit(const std::string& id, const Rect& rect) {
    if (record_hits) hits_[id] = rect;
}

std::optional<UI::HitRect> UI::rect_of(const std::string& id) const {
    const auto found = hits_.find(id);
    if (found == hits_.end()) return std::nullopt;
    return HitRect{found->second.x, found->second.y, found->second.w, found->second.h};
}

void UI::load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    fonts_found_ = find_fonts(i18n::language());
    const auto load = [&io](const std::vector<FontFile>& chain) -> ImFont* {
        ImFont* font = nullptr;
        for (const FontFile& file : chain) {
            const FontData* data = font_data(file.path);
            if (data == nullptr || data->size > 0x7FFFFFFF) continue;
            ImFontConfig config;
            config.FontNo = static_cast<ImU32>(file.index);
            config.MergeMode = font != nullptr;    // 後面的字型補前面沒有的字（中文、日文）
            config.FontDataOwnedByAtlas = false;   // 字型檔是對應到記憶體的，一直都在，不必複製一份
            // Dear ImGui 的字級指的是「行高」（上緣到下緣），而各個字型的行高差很多：Noto Sans CJK 的行高是字身的 1.45 倍，
            // Arial 是 1.12 倍，同一個字級畫出來中文會比英文小一截。每個字型各放大「行高÷字身」倍，
            // 字級就變成字身的大小（和 macOS 版的字級同一個意思），接在一起的幾個字型畫出來一樣大。
            config.ExtraSizeScale = font_line_height_per_em(*data, file.index);
            ImFont* added = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data->bytes), static_cast<int>(data->size), 14.0f, &config);
            if (font == nullptr) font = added;
        }
        return font;
    };
    regular_ = load(fonts_found_.regular);
    if (regular_ == nullptr) regular_ = io.Fonts->AddFontDefault();   // 一個字型檔都找不到：用 Dear ImGui 內附的（只有英文字母）
    bold_ = load(fonts_found_.bold);
    if (bold_ == nullptr) bold_ = regular_;
    mono_ = load(fonts_found_.mono);
    if (mono_ == nullptr) mono_ = regular_;
    mono_bold_ = load(fonts_found_.mono_bold);
    if (mono_bold_ == nullptr) mono_bold_ = mono_;
    io.FontDefault = regular_;
    CanvasLayer::set_fonts(fonts_found_.bold);
    VideoStamp::set_fonts(fonts_found_.mono_bold);
}

// ---------------------------------------------------------------- 元件

void UI::text(ImDrawList* dl, ImFont* font, float size, float x, float y, std::uint32_t color, const std::string& value) const {
    dl->AddText(font, S(size), ImVec2(std::round(x), std::round(y)), color, value.c_str());
}

float UI::text_width(ImFont* font, float size, const std::string& value) const {
    return font->CalcTextSizeA(S(size), FLT_MAX, 0.0f, value.c_str()).x;
}

std::string UI::fitted(ImFont* font, float size, const std::string& value, float max_width) const {
    if (text_width(font, size, value) <= max_width) return value;
    // 一次去掉一個字（UTF-8 的一個字可能是好幾個位元組）
    std::string cut = value;
    while (!cut.empty()) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
        if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0x80) != 0 && (static_cast<unsigned char>(cut.back()) & 0xC0) != 0xC0) continue;
        std::string candidate = cut;
        // 去掉了一個多位元組字的尾巴之後，開頭的那個位元組也要拿掉
        while (!candidate.empty() && (static_cast<unsigned char>(candidate.back()) & 0xC0) == 0xC0) candidate.pop_back();
        cut = candidate;
        if (text_width(font, size, cut + "…") <= max_width) return cut + "…";
    }
    return "…";
}

// 一段字照寬度折成幾行。中文、日文可以在任何兩個字之間換行（標點不放在行首，開頭的括號不放在行尾）；
// 英文、西班牙文只在空白的地方換行。Dear ImGui 自己的折行只認空白，中文一長串會整段擠到下一行。
std::vector<std::string> UI::wrap(ImFont* font, float size, const std::string& value, float max_width) const {
    // 先切成一個一個「不能從中間斷開」的小段
    struct Unit {
        std::string text;
        bool space = false;    // 是空白（行首行尾的空白不畫）
        bool newline = false;  // 原文裡的換行
    };
    const auto is_cjk = [](char32_t c) { return c >= 0x2E80 && !(c >= 0xFF61 && c <= 0xFF9F); };
    const auto closes = [](char32_t c) {
        for (const char32_t p : U"，。、；：！？）」』》〉】…,.;:!?)") {
            if (p != 0 && p == c) return true;
        }
        return false;
    };
    const auto opens = [](char32_t c) {
        for (const char32_t p : U"（「『《〈【(") {
            if (p != 0 && p == c) return true;
        }
        return false;
    };
    std::vector<Unit> units;
    char32_t previous = 0;   // 上一個字；0 表示這是開頭，或上一個是空白、換行
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        char32_t code = lead < 0x80 ? lead : lead < 0xE0 ? (lead & 0x1F) : lead < 0xF0 ? (lead & 0x0F) : (lead & 0x07);
        for (std::size_t k = 1; k < length && i + k < value.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(value[i + k]) & 0x3F);
        const std::string piece = value.substr(i, length);
        i += length;
        if (code == '\n') {
            units.push_back({"", false, true});
            previous = 0;
            continue;
        }
        if (code == ' ') {
            units.push_back({" ", true, false});
            previous = 0;
            continue;
        }
        // 和上一個字黏在一起（中間不能換行）：上一個字是開頭的括號、這個字是收尾的標點，或兩個都不是中日文（同一個單字）
        const bool attach = previous != 0 && (opens(previous) || closes(code) || (!is_cjk(previous) && !is_cjk(code)));
        if (attach) units.back().text += piece;
        else units.push_back({piece, false, false});
        previous = code;
    }
    std::vector<std::string> lines;
    std::string line;
    const auto flush = [&] {
        while (!line.empty() && line.back() == ' ') line.pop_back();
        lines.push_back(line);
        line.clear();
    };
    for (const Unit& unit : units) {
        if (unit.newline) {
            flush();
            continue;
        }
        if (unit.space) {
            if (!line.empty()) line += " ";
            continue;
        }
        if (!line.empty() && text_width(font, size, line + unit.text) > max_width) flush();
        line += unit.text;
    }
    if (!line.empty() || lines.empty()) flush();
    return lines;
}

float UI::paragraph_height(ImFont* font, float size, const std::string& value, float max_width) const {
    return static_cast<float>(wrap(font, size, value, max_width).size()) * S(size) * kLineSpacing;
}

/// 畫一段會折行的字，回傳畫了多高。centered：每一行各自置中。
float UI::paragraph(ImDrawList* dl, ImFont* font, float size, float x, float y, float max_width, std::uint32_t color, const std::string& value, bool centered) const {
    const std::vector<std::string> lines = wrap(font, size, value, max_width);
    const float line_height = S(size) * kLineSpacing;
    float line_y = y + (line_height - S(size)) / 2;
    for (const std::string& line : lines) {
        text(dl, font, size, centered ? x + (max_width - text_width(font, size, line)) / 2 : x, line_y, color, line);
        line_y += line_height;
    }
    return static_cast<float>(lines.size()) * line_height;
}

// 版面稿裡的線條圖示，照 macOS 版 Theme.swift 的路徑重畫。(x, y, w, h) 是顯示的範圍。
void UI::icon(ImDrawList* dl, Icon kind, float x, float y, float w, float h, std::uint32_t color, float line_width) const {
    const bool small = kind == Icon::pause || kind == Icon::play || kind == Icon::previous || kind == Icon::next;
    const float box_w = kind == Icon::chevrons ? 12.0f : small ? 12.0f : 16.0f;
    const float box_h = kind == Icon::chevrons ? 14.0f : small ? 12.0f : 16.0f;
    const float scale = std::min(w / box_w, h / box_h);
    const float ox = x + (w - box_w * scale) / 2, oy = y + (h - box_h * scale) / 2;
    const auto p = [&](float px, float py) { return ImVec2(ox + px * scale, oy + py * scale); };
    const float t = std::max(1.0f, line_width * scale);
    const auto line = [&](float x1, float y1, float x2, float y2) { dl->AddLine(p(x1, y1), p(x2, y2), color, t); };
    const auto poly = [&](std::initializer_list<ImVec2> points, bool closed) {
        dl->AddPolyline(points.begin(), static_cast<int>(points.size()), color, t, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    };
    switch (kind) {
    case Icon::settings:
        line(2, 4.5f, 8, 4.5f);
        line(12, 4.5f, 14, 4.5f);
        line(2, 11.5f, 4, 11.5f);
        line(8, 11.5f, 14, 11.5f);
        dl->AddCircle(p(10, 4.5f), 2 * scale, color, 0, t);
        dl->AddCircle(p(6, 11.5f), 2 * scale, color, 0, t);
        break;
    case Icon::download:
        line(8, 2, 8, 10);
        poly({p(4.5f, 7), p(8, 10.5f), p(11.5f, 7)}, false);
        line(2.5f, 13.5f, 13.5f, 13.5f);
        break;
    case Icon::camera:
        dl->AddRect(p(1.5f, 4), p(10.5f, 12), color, 1.5f * scale, t);
        poly({p(10.5f, 7), p(14.5f, 5), p(14.5f, 11), p(10.5f, 9)}, true);
        break;
    case Icon::chevrons:
        poly({p(3, 5.5f), p(6, 2.5f), p(9, 5.5f)}, false);
        poly({p(3, 8.5f), p(6, 11.5f), p(9, 8.5f)}, false);
        break;
    case Icon::lock:
        dl->AddRect(p(3.5f, 7), p(12.5f, 13.5f), color, 1.5f * scale, t);
        dl->PathLineTo(p(5.5f, 7));
        dl->PathLineTo(p(5.5f, 5));
        dl->PathArcTo(p(8, 5), 2.5f * scale, 3.14159265f, 2 * 3.14159265f);
        dl->PathLineTo(p(10.5f, 7));
        dl->PathStroke(color, t);
        break;
    case Icon::video_file:
        dl->AddRect(p(2, 2.5f), p(14, 13.5f), color, 1.5f * scale, t);
        poly({p(6.6f, 5.8f), p(6.6f, 10.2f), p(10.4f, 8)}, true);
        break;
    case Icon::pause:
        dl->AddRectFilled(p(2, 1.5f), p(5, 10.5f), color, 0.8f * scale);
        dl->AddRectFilled(p(7, 1.5f), p(10, 10.5f), color, 0.8f * scale);
        break;
    case Icon::play:
        dl->AddTriangleFilled(p(3, 1.5f), p(10.5f, 6), p(3, 10.5f), color);
        break;
    case Icon::check:
        poly({p(3, 8.5f), p(6.5f, 12), p(13, 4.5f)}, false);
        break;
    case Icon::clock:
        dl->AddCircle(p(8, 8), 6 * scale, color, 0, t);
        poly({p(8, 4.8f), p(8, 8), p(10.3f, 9.5f)}, false);
        break;
    case Icon::image_file:   // 照影片檔那一個的外框畫的：框裡是太陽和山的輪廓
        dl->AddRect(p(2, 2.5f), p(14, 13.5f), color, 1.5f * scale, t);
        dl->AddCircle(p(5.8f, 6.1f), 1.1f * scale, color, 0, t);
        poly({p(2.6f, 11.4f), p(6.3f, 8.6f), p(8.6f, 10.4f), p(10.6f, 8.8f), p(13.4f, 11.2f)}, false);
        break;
    case Icon::previous:
        poly({p(7.5f, 2.5f), p(4, 6), p(7.5f, 9.5f)}, false);
        break;
    case Icon::next:
        poly({p(4.5f, 2.5f), p(8, 6), p(4.5f, 9.5f)}, false);
        break;
    }
}

void UI::tooltip(const std::string& value) {
    if (value.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(6));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenu);
    ImGui::PushStyleColor(ImGuiCol_Border, kButtonRing);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    if (ImGui::BeginTooltip()) {
        ImGui::PushFont(regular_, 12.0f * scale_);
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        ImGui::PushTextWrapPos(S(340));
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

/// 一般的按鈕：圓角的深色底加一圈邊。leading 是字前面的圖示，trailing 是字後面比較淡的一小段（「3 張」）。
bool UI::button(const char* id, const Rect& rect, const std::string& label, bool enabled, Icon* leading, const std::string& trailing) {
    note_hit(id, rect);
    ImGui::SetCursorScreenPos(ImVec2(rect.x, rect.y));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(rect.w, rect.h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), enabled ? (hovered ? kButtonHover : kButton) : kDisabledFill, S(7));
    dl->AddRect(ImVec2(rect.x + 0.5f, rect.y + 0.5f), ImVec2(rect.right() - 0.5f, rect.bottom() - 0.5f), enabled ? kButtonRing : kSeparator, S(7));
    const ImU32 ink = enabled ? kText : kTertiary;
    const float icon_w = leading != nullptr ? S(14) + S(6) : 0;
    const float trailing_w = trailing.empty() ? 0 : text_width(mono_, 11.5f, trailing) + S(7);
    const float label_w = text_width(regular_, 12.5f, label);
    float x = rect.x + (rect.w - icon_w - label_w - trailing_w) / 2;
    if (leading != nullptr) {
        icon(dl, *leading, x, rect.y + (rect.h - S(14)) / 2, S(14), S(14), ink);
        x += icon_w;
    }
    text(dl, regular_, 12.5f, x, rect.y + (rect.h - S(12.5f)) / 2 - S(1.5f), ink, label);
    if (!trailing.empty()) text(dl, mono_, 11.5f, x + label_w + S(7), rect.y + (rect.h - S(11.5f)) / 2 - S(1), kSecondary, trailing);
    return clicked && enabled;
}

/// 分段選擇（鏡頭／影片檔／圖片序列、填滿／完整、上／下）。回傳選到第幾段。
int UI::segmented(const char* id, const Rect& rect, const std::vector<std::string>& labels, int selected, bool enabled) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kWell, S(7));
    const float pad = S(2), width = (rect.w - 2 * pad) / static_cast<float>(labels.size());
    int result = selected;
    ImGui::PushID(id);
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const Rect cell{rect.x + pad + width * static_cast<float>(i), rect.y + pad, width, rect.h - 2 * pad};
        note_hit(std::string(id) + "/" + std::to_string(i), cell);
        ImGui::SetCursorScreenPos(ImVec2(cell.x, cell.y));
        ImGui::BeginDisabled(!enabled);
        if (ImGui::InvisibleButton(labels[static_cast<std::size_t>(i)].c_str(), ImVec2(cell.w, cell.h))) result = i;
        ImGui::EndDisabled();
        const bool active = i == selected;
        if (active) dl->AddRectFilled(ImVec2(cell.x, cell.y), ImVec2(cell.right(), cell.bottom()), rgb(0x3A424C), S(5.5f));
        ImFont* font = active ? bold_ : regular_;
        // 選項多、名稱又長的時候（別的語言）字略縮，不要蓋到隔壁那一格；最多縮到八成
        float size = 12.5f;
        const float room = cell.w - S(8), natural = text_width(bold_, size, labels[static_cast<std::size_t>(i)]);
        if (natural > room && natural > 0) size = std::max(10.0f, size * room / natural);
        const float w = text_width(font, size, labels[static_cast<std::size_t>(i)]);
        text(dl, font, size, cell.x + (cell.w - w) / 2, cell.y + (cell.h - S(size)) / 2 - S(1.5f), active ? kText : kSecondary, labels[static_cast<std::size_t>(i)]);
    }
    ImGui::PopID();
    return result;
}

/// 打勾的方格加文字。回傳有沒有被按（值由呼叫的人自己改）。
bool UI::checkbox(const char* id, float x, float y, const std::string& label, bool value) {
    const float box = S(15), gap = S(8);
    const float width = box + (label.empty() ? 0 : gap + text_width(regular_, 12.5f, label));
    note_hit(id, Rect{x, y, width, std::max(box, S(18))});
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, std::max(box, S(18))));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float by = y + (std::max(box, S(18)) - box) / 2;
    if (value) {
        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + box, by + box), kRx, S(3.5f));
        icon(dl, Icon::check, x + S(1.5f), by + S(1.5f), box - S(3), box - S(3), rgb(0x0C1A1C), 2.2f);
    } else {
        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + box, by + box), kWell, S(3.5f));
        dl->AddRect(ImVec2(x + 0.5f, by + 0.5f), ImVec2(x + box - 0.5f, by + box - 0.5f), kButtonRing, S(3.5f));
    }
    if (!label.empty()) text(dl, regular_, 12.5f, x + box + gap, by + (box - S(12.5f)) / 2 - S(1.5f), kText, label);
    return clicked;
}

/// 按了會跳出選單的鈕（右邊有上下兩個箭頭；locked 時換成鎖頭、不能按）。
bool UI::popup_button(const char* id, const Rect& rect, const std::string& label, bool enabled, Icon* leading, bool locked) {
    note_hit(id, rect);
    ImGui::SetCursorScreenPos(ImVec2(rect.x, rect.y));
    ImGui::BeginDisabled(!enabled || locked);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(rect.w, rect.h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool usable = enabled && !locked;
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), usable ? (hovered ? kButtonHover : kButton) : kDisabledFill, S(7));
    dl->AddRect(ImVec2(rect.x + 0.5f, rect.y + 0.5f), ImVec2(rect.right() - 0.5f, rect.bottom() - 0.5f), usable ? kButtonRing : kSeparator, S(7));
    float x = rect.x + S(10);
    if (leading != nullptr) {
        icon(dl, *leading, x, rect.y + (rect.h - S(14)) / 2, S(14), S(14), usable ? kText : kTertiary);
        x += S(14) + S(7);
    }
    const float right = rect.right() - S(10) - S(12);
    text(dl, regular_, 12.5f, x, rect.y + (rect.h - S(12.5f)) / 2 - S(1.5f), usable ? kText : kTertiary, fitted(regular_, 12.5f, label, right - x - S(6)));
    icon(dl, locked ? Icon::lock : Icon::chevrons, right, rect.y + (rect.h - S(13)) / 2, S(12), S(13), kTertiary);
    return clicked && usable;
}

/// 橫的滑桿。拖動時改 value，回傳這一圈有沒有改。
bool UI::slider(const char* id, const Rect& rect, double& value, double min, double max, std::uint32_t color) {
    note_hit(id, rect);
    ImGui::SetCursorScreenPos(ImVec2(rect.x, rect.y));
    ImGui::InvisibleButton(id, ImVec2(rect.w, rect.h));
    bool changed = false;
    if (ImGui::IsItemActive() && rect.w > 0) {
        const double fraction = std::clamp(static_cast<double>((ImGui::GetIO().MousePos.x - rect.x) / rect.w), 0.0, 1.0);
        const double next = min + (max - min) * fraction;
        changed = next != value;
        value = next;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = rect.y + rect.h / 2, track = S(4);
    const float fraction = static_cast<float>(std::clamp((value - min) / (max - min), 0.0, 1.0));
    const float knob_x = rect.x + rect.w * fraction;
    dl->AddRectFilled(ImVec2(rect.x, cy - track / 2), ImVec2(rect.right(), cy + track / 2), kTrack, track / 2);
    dl->AddRectFilled(ImVec2(rect.x, cy - track / 2), ImVec2(knob_x, cy + track / 2), color, track / 2);
    dl->AddCircleFilled(ImVec2(knob_x, cy), S(6.5f), color);
    return changed;
}

void UI::level_bar(ImDrawList* dl, const Rect& rect, double fraction, std::uint32_t color) const {
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kTrack, rect.h / 2);
    const float w = rect.w * static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    if (w > 1) dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.x + w, rect.bottom()), color, rect.h / 2);
}

/// 輸入框。回傳內容有沒有改。
bool UI::input_text(const char* id, const Rect& rect, std::string& value, const std::string& hint, bool mono, bool uppercase, std::size_t max_length) {
    note_hit(id, rect);
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, "%s", value.c_str());
    const float font_size = 13.0f;
    ImGui::PushFont(mono ? mono_ : regular_, font_size * scale_);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(9), std::max(0.0f, (rect.h - ImGui::GetFontSize()) / 2)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kWell);
    ImGui::PushStyleColor(ImGuiCol_Border, kButtonRing);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, kTertiary);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, rgb(0x44D6D6, 90));
    ImGui::SetCursorScreenPos(ImVec2(rect.x, rect.y));
    ImGui::SetNextItemWidth(rect.w);
    const ImGuiInputTextFlags flags = uppercase ? ImGuiInputTextFlags_CharsUppercase : ImGuiInputTextFlags_None;
    const std::size_t capacity = max_length > 0 ? std::min(sizeof buffer, max_length + 1) : sizeof buffer;
    const bool changed = ImGui::InputTextWithHint(id, hint.c_str(), buffer, capacity, flags);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    if (changed) value = buffer;
    return changed;
}

SDL_Texture* UI::texture_for(const PicturePtr& picture) {
    if (!picture || renderer_ == nullptr) return nullptr;
    auto found = textures_.find(picture.get());
    if (found == textures_.end()) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, kImageWidth, kImageHeight);
        if (texture == nullptr) return nullptr;
        SDL_UpdateTexture(texture, nullptr, picture->rgb.data(), kImageWidth * 3);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);   // 96×72 的像素原樣放大，不內插
        found = textures_.emplace(picture.get(), CachedTexture{texture, frame_counter_, picture}).first;
    }
    found->second.used = frame_counter_;
    return found->second.texture;
}

/// 畫一張 96×72 的畫面：像素原樣放大，不內插。沒有畫面時畫一塊底色。
/// Dear ImGui 的 SDL 繪圖後端預設把每一張貼圖都平滑內插，所以前後各放一個它提供的回呼，只有這一張改成不內插。
void UI::picture(ImDrawList* dl, const PicturePtr& picture, const Rect& rect, float rounding, int round_flags) {
    SDL_Texture* texture = texture_for(picture);
    if (texture == nullptr) {
        dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kPlaceholder, rounding, round_flags);
        return;
    }
    const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    if (platform.DrawCallback_SetSamplerNearest != nullptr) dl->AddCallback(platform.DrawCallback_SetSamplerNearest);
    dl->AddImageRounded(tex(texture), ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding, round_flags);
    if (platform.DrawCallback_SetSamplerLinear != nullptr) dl->AddCallback(platform.DrawCallback_SetSamplerLinear);
}

void UI::update_waterfall_texture() {
    if (renderer_ == nullptr) return;
    if (waterfall_texture_ == nullptr) {
        waterfall_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, WaterfallStore::kWidth, WaterfallStore::kHeight);
        if (waterfall_texture_ == nullptr) return;
        SDL_SetTextureScaleMode(waterfall_texture_, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(waterfall_texture_, SDL_BLENDMODE_NONE);
        waterfall_version_ = static_cast<std::uint64_t>(-1);
    }
    if (waterfall_version_ == app_.waterfall.version()) return;
    waterfall_version_ = app_.waterfall.version();
    SDL_UpdateTexture(waterfall_texture_, nullptr, app_.waterfall.pixels().data(), WaterfallStore::kWidth * 4);
}

/// 「12.3 dB」的小標籤，疊在收到的畫面左上角。
void UI::draw_snr_badge(ImDrawList* dl, float x, float y, double snr_db, float size) const {
    const std::string label = App::snr_text(snr_db);
    const float w = text_width(mono_, size, label) + S(size * 0.7f), h = S(size * 1.55f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), rgb(0x000000, 150), S(3));
    text(dl, mono_, size, x + (w - text_width(mono_, size, label)) / 2, y + (h - S(size)) / 2 - S(size * 0.08f), kText, label);
}

// ---------------------------------------------------------------- 整個視窗

void UI::draw(float width, float height) {
    ++frame_counter_;
    hits_.clear();
    // 很久沒用到的貼圖還回去（「最近收到」最多 5000 張，只留看得到的那些）
    if (textures_.size() > 96) {
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (frame_counter_ - it->second.used > 120) {
                SDL_DestroyTexture(it->second.texture);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
    }
    update_waterfall_texture();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(width, height), kBackground);

    const float top = S(52), bottom = S(28), side = std::min(S(320), width * 0.36f);
    const Rect top_bar{0, 0, width, top}, bottom_bar{0, height - bottom, width, bottom};
    const Rect aside{width - side, top, side, height - top - bottom};
    const Rect main{0, top, width - side, height - top - bottom};

    // 主要的區域：上面是畫面與兩個面板，下面是瀑布圖
    const float pad = S(20), gap = S(14);
    const float spectrum_h = std::clamp(main.h * 0.30f, S(150), S(250));
    const float upper_h = main.h - 2 * pad - gap - spectrum_h;
    const float content_w = main.w - 2 * pad;
    // 畫面是 4:3，下面有一條 46 高的說明列；高度不夠時畫面縮小，多出來的寬度給右邊的面板
    const float caption = S(46);
    float picture_w = content_w - gap - std::clamp(content_w * 0.36f, S(260), S(360));
    picture_w = std::min(picture_w, (upper_h - caption) * 4 / 3);
    picture_w = std::max(picture_w, S(240));
    const Rect picture_panel{main.x + pad, main.y + pad, picture_w, picture_w * 3 / 4 + caption};
    const float column_x = picture_panel.right() + gap, column_w = main.x + main.w - pad - column_x;
    const float signal_h = std::min(S(224), picture_panel.h * 0.47f);
    const Rect signal_panel{column_x, picture_panel.y, column_w, signal_h};
    const Rect recent_panel{column_x, picture_panel.y + signal_h + S(12), column_w, picture_panel.h - signal_h - S(12)};
    const Rect spectrum{main.x + pad, picture_panel.bottom() + gap, content_w, main.y + main.h - pad - (picture_panel.bottom() + gap)};

    draw_picture_panel(picture_panel);
    draw_signal_panel(signal_panel);
    draw_recent_panel(recent_panel);
    draw_spectrum(spectrum);
    draw_side_panel(aside);
    draw_top_bar(top_bar);
    draw_bottom_bar(bottom_bar);
    if (app_.ptt_warning) draw_ptt_warning(Rect{main.x + pad, main.y + S(10), content_w, 0});

    draw_mode_menu();
    draw_schedule_menu();
    draw_save_video_menu();
    draw_settings();
    ImGui::End();
}

void UI::draw_top_bar(const Rect& rect) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kBar);
    dl->AddLine(ImVec2(rect.x, rect.bottom() - 0.5f), ImVec2(rect.right(), rect.bottom() - 0.5f), kBarBorder);
    const float cy = rect.y + rect.h / 2;
    text(dl, bold_, 14.5f, rect.x + S(20), cy - S(9.5f), kText, L("短波 NBTV"));

    // 中間的狀態：接收中／準備發射／發射中
    std::string state, detail;
    ImU32 dot = kRx, fill = kRxPill, ring = kRxPillRing, ink = kRx;
    if (app_.tx_phase == App::TxPhase::starting) {
        state = L("準備發射");
        dot = kTx;
        ink = kTx;
        fill = rgb(0x2B2118);
        ring = rgb(0x6E4A2C);
    } else if (app_.is_on_air()) {
        state = L("發射中");
        detail = App::clock_text(app_.tx_elapsed);
        fill = kTx;
        ring = kTx;
        ink = kTxInk;
        dot = kTxInk;
    } else {
        state = L("接收中");
        detail = !app_.synced ? L("等待訊號") : app_.sync_callsign.empty() ? L("已同步") : LF("已同步 %@", {app_.sync_callsign});
    }
    const bool on_air = app_.is_on_air();
    const float state_w = text_width(bold_, 13.5f, state);
    const float detail_w = detail.empty() ? 0 : text_width(on_air ? mono_bold_ : regular_, 13.5f, detail) + S(8);
    const float pill_w = S(14) + S(8) + S(8) + state_w + detail_w + S(14), pill_h = S(30);
    const float px = rect.x + (rect.w - pill_w) / 2, py = cy - pill_h / 2;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pill_w, py + pill_h), fill, pill_h / 2);
    dl->AddRect(ImVec2(px + 0.5f, py + 0.5f), ImVec2(px + pill_w - 0.5f, py + pill_h - 0.5f), ring, pill_h / 2);
    dl->AddCircleFilled(ImVec2(px + S(14) + S(4), cy), S(4), dot);
    text(dl, bold_, 13.5f, px + S(14) + S(16), cy - S(9), ink, state);
    if (!detail.empty()) text(dl, on_air ? mono_bold_ : regular_, 13.5f, px + S(14) + S(16) + state_w + S(8), cy - S(9), on_air ? kTxInk : kText, detail);

    // 右邊：設定鈕、定時發射鈕、我的呼號
    float x = rect.right() - S(16);
    const Rect settings_button{x - S(36), cy - S(15), S(36), S(30)};
    note_hit("##settings", settings_button);
    ImGui::SetCursorScreenPos(ImVec2(settings_button.x, settings_button.y));
    if (ImGui::InvisibleButton("##settings", ImVec2(settings_button.w, settings_button.h))) show_settings = !show_settings;
    const bool settings_hovered = ImGui::IsItemHovered();
    tooltip(L("設定"));
    dl->AddRectFilled(ImVec2(settings_button.x, settings_button.y), ImVec2(settings_button.right(), settings_button.bottom()), settings_hovered ? kButtonHover : kButton, S(7));
    dl->AddRect(ImVec2(settings_button.x + 0.5f, settings_button.y + 0.5f), ImVec2(settings_button.right() - 0.5f, settings_button.bottom() - 0.5f), kButtonRing, S(7));
    icon(dl, Icon::settings, settings_button.x + (settings_button.w - S(16)) / 2, settings_button.y + (settings_button.h - S(16)) / 2, S(16), S(16), kText);
    x = settings_button.x - S(10);

    const std::string summary = app_.schedule_summary();
    const bool armed = app_.schedule_armed || app_.scheduled_run;
    const float schedule_w = S(12) + S(14) + S(6) + text_width(regular_, 12.5f, summary) + S(12);
    const Rect schedule_button{x - schedule_w, cy - S(15), schedule_w, S(30)};
    note_hit("##schedule", schedule_button);
    ImGui::SetCursorScreenPos(ImVec2(schedule_button.x, schedule_button.y));
    if (ImGui::InvisibleButton("##schedule", ImVec2(schedule_button.w, schedule_button.h))) want_schedule_menu_ = true;
    const bool schedule_hovered = ImGui::IsItemHovered();
    schedule_anchor_ = schedule_button;
    dl->AddRectFilled(ImVec2(schedule_button.x, schedule_button.y), ImVec2(schedule_button.right(), schedule_button.bottom()),
                      armed ? kTx : (schedule_hovered ? kButtonHover : kButton), S(7));
    if (!armed) dl->AddRect(ImVec2(schedule_button.x + 0.5f, schedule_button.y + 0.5f), ImVec2(schedule_button.right() - 0.5f, schedule_button.bottom() - 0.5f), kButtonRing, S(7));
    icon(dl, Icon::clock, schedule_button.x + S(12), cy - S(7), S(14), S(14), armed ? kTxInk : kSecondary);
    text(dl, armed ? bold_ : regular_, 12.5f, schedule_button.x + S(12) + S(20), cy - S(8), armed ? kTxInk : kText, summary);
    x = schedule_button.x - S(16);

    const std::string call = app_.settings.callsign.empty() ? L("未設定") : app_.settings.callsign;
    const float call_w = text_width(mono_bold_, 13.5f, call);
    text(dl, mono_bold_, 13.5f, x - call_w, cy - S(9), app_.settings.callsign.empty() ? kTertiary : kText, call);
    const std::string mine = L("我的呼號");
    text(dl, regular_, 12, x - call_w - S(9) - text_width(regular_, 12, mine), cy - S(8), kSecondary, mine);
}

void UI::draw_bottom_bar(const Rect& rect) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kBar);
    dl->AddLine(ImVec2(rect.x, rect.y + 0.5f), ImVec2(rect.right(), rect.y + 0.5f), kBarBorder);
    const float y = rect.y + (rect.h - S(11.5f)) / 2 - S(1.5f);
    float x = rect.x + S(16);
    const auto pair = [&](const std::string& label, const std::string& value) {
        text(dl, regular_, 11.5f, x, y, kTertiary, label);
        x += text_width(regular_, 11.5f, label) + S(10);
        const std::string shown = fitted(regular_, 11.5f, value, S(220));
        text(dl, regular_, 11.5f, x, y, kText, shown);
        x += text_width(regular_, 11.5f, shown) + S(22);
    };
    pair(L("音訊輸入"), app_.input_device_name);
    pair(L("音訊輸出"), app_.output_device_name);
    pair("PTT", app_.settings.ptt_summary());
    if (app_.notice) {
        // 要讓使用者看到的訊息：放在右邊，按一下就關掉
        const float available = rect.right() - S(16) - x;
        if (available > S(60)) {
            const std::string shown = fitted(regular_, 11.5f, *app_.notice, available);
            const float w = text_width(regular_, 11.5f, shown);
            ImGui::SetCursorScreenPos(ImVec2(rect.right() - S(16) - w, rect.y));
            const bool dismiss = ImGui::InvisibleButton("##notice", ImVec2(w, rect.h));
            tooltip(*app_.notice + "\n" + L("按一下關閉這則訊息"));
            text(dl, regular_, 11.5f, rect.right() - S(16) - w, y, kTx, shown);
            if (dismiss) app_.notice.reset();
        }
    }
}

/// PTT 的指令沒有成功、電台可能還按著：主視窗上方疊一條橘色的警告，按了「知道了」才消失。
void UI::draw_ptt_warning(const Rect& rect) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const std::string title = L("PTT 可能還按著"), body = *app_.ptt_warning, ok = L("知道了");
    const float ok_w = text_width(bold_, 12.5f, ok) + S(28);
    const float wrap = rect.w - S(32) - ok_w - S(16);
    const float body_h = paragraph_height(regular_, 12.5f, body, wrap);
    const float h = S(12) + S(22) + body_h + S(12);
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.y + h), kTx, S(9));
    text(dl, bold_, 13.5f, rect.x + S(16), rect.y + S(12), kTxInk, title);
    paragraph(dl, regular_, 12.5f, rect.x + S(16), rect.y + S(12) + S(22), wrap, kTxInk, body);
    const Rect button_rect{rect.right() - S(16) - ok_w, rect.y + (h - S(30)) / 2, ok_w, S(30)};
    note_hit("##pttwarning", button_rect);
    dl->AddRectFilled(ImVec2(button_rect.x, button_rect.y), ImVec2(button_rect.right(), button_rect.bottom()), kTxInk, S(7));
    text(dl, bold_, 12.5f, button_rect.x + (ok_w - text_width(bold_, 12.5f, ok)) / 2, button_rect.y + S(7), kTx, ok);
    ImGui::SetCursorScreenPos(ImVec2(button_rect.x, button_rect.y));
    if (ImGui::InvisibleButton("##pttwarning", ImVec2(button_rect.w, button_rect.h))) app_.ptt_warning.reset();
}

// ---------------------------------------------------------------- 左邊：畫面、訊號、最近收到、瀑布圖

void UI::draw_picture_panel(const Rect& rect) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool on_air = app_.is_on_air(), starting = app_.tx_phase == App::TxPhase::starting;
    const float radius = S(10), caption = S(46);
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kPanel, radius);
    const Rect image{rect.x + 1, rect.y + 1, rect.w - 2, rect.h - caption - 1};
    dl->AddRectFilled(ImVec2(image.x, image.y), ImVec2(image.right(), image.bottom()), kWell, radius, ImDrawFlags_RoundTop);
    const float caption_y = rect.bottom() - caption;
    const float text_y = caption_y + (caption - S(17)) / 2 - S(2);

    const auto placeholder = [&](const std::string& title, const std::string& body) {
        const float tw = text_width(bold_, 15, title), bw = text_width(regular_, 12, body);
        text(dl, bold_, 15, image.x + (image.w - tw) / 2, image.y + image.h / 2 - S(20), kText, title);
        text(dl, regular_, 12, image.x + (image.w - bw) / 2, image.y + image.h / 2 + S(4), kSecondary, body);
    };

    if (on_air || starting) {
        // 發射：正在送出的那一張（壓縮後對方會看到的樣子）
        if (app_.tx_preview) {
            picture(dl, app_.tx_preview, image, radius, ImDrawFlags_RoundTop);
        } else {
            placeholder(L("準備發射"), L("正在處理第一張畫面"));
        }
        dl->AddRect(ImVec2(rect.x + 1, rect.y + 1), ImVec2(rect.right() - 1, rect.bottom() - 1), kTx, radius, S(2));
        float x = rect.x + S(14);
        text(dl, mono_bold_, 17, x, text_y, kText, app_.settings.callsign);
        x += text_width(mono_bold_, 17, app_.settings.callsign) + S(14);
        if (const ModeInfo* mode = app_.tx_mode()) {
            const std::string info = app_.tx_frame_number > 0 ? mode->label() + " · " + LF("第 %d 張", {app_.tx_frame_number}) : mode->label();
            text(dl, regular_, 12, x, caption_y + (caption - S(12)) / 2 - S(1.5f), kSecondary, info);
        }
        const std::string note = L("對方收到的樣子（沒有雜訊時）");
        text(dl, regular_, 12, rect.right() - S(14) - text_width(regular_, 12, note), caption_y + (caption - S(12)) / 2 - S(1.5f), kSecondary, note);
        return;
    }

    const App::ReceivedFrame* frame = app_.displayed_frame();
    if (frame != nullptr) {
        picture(dl, frame->picture, image, radius, ImDrawFlags_RoundTop);
        if (app_.settings.show_frame_snr) draw_snr_badge(dl, image.x + S(10), image.y + S(10), frame->snr_db, 13);
    } else {
        placeholder(L("等待訊號"), L("偵測到同步訊號後會自動解碼並顯示畫面"));
    }

    // 說明列右邊的兩個按鈕
    Icon download = Icon::download, video = Icon::video_file;
    const std::vector<App::ReceivedFrame> sequence = app_.displayed_sequence();
    const std::string save_image_label = L("儲存圖片"), save_video_label = app_.exporting_video ? L("儲存中…") : L("儲存影片");
    const std::string count = sequence.size() >= 2 && !app_.exporting_video ? LF("%d 張", {static_cast<int>(sequence.size())}) : std::string();
    const float image_w = S(14) + S(6) + text_width(regular_, 12.5f, save_image_label) + S(24);
    const float video_w = S(14) + S(6) + text_width(regular_, 12.5f, save_video_label) + (count.empty() ? 0 : text_width(mono_, 11.5f, count) + S(7)) + S(24);
    const Rect image_button{rect.right() - S(12) - image_w, caption_y + (caption - S(30)) / 2, image_w, S(30)};
    const Rect video_button{image_button.x - S(8) - video_w, image_button.y, video_w, S(30)};
    const bool can_save_video = sequence.size() >= 2 && !app_.exporting_video && FileSource::ffmpeg_available();
    // 按了先跳出一小塊選項（要不要印上訊雜比），在那裡按「儲存…」才開存檔的對話框（見 draw_save_video_menu）
    save_video_anchor_ = video_button;
    if (button("##savevideo", video_button, save_video_label, can_save_video, &video, count)) want_save_video_menu_ = true;
    if (app_.exporting_video) tooltip(L("正在存影片檔"));
    else if (sequence.size() < 2) tooltip(LF("這一張前後沒有連續收到的畫面：同一個呼號、空檔不到 %d 秒的才能串成影片", {static_cast<int>(App::kSequenceGapSeconds)}));
    else if (!FileSource::ffmpeg_available()) tooltip(L("要存成影片，這台電腦要先裝 ffmpeg"));
    else tooltip(LF("把這一段連續收到的 %d 張串成影片檔（.mp4）：同一個呼號，兩張之間的空檔不到 %d 秒", {static_cast<int>(sequence.size()), static_cast<int>(App::kSequenceGapSeconds)}));
    if (button("##saveimage", image_button, save_image_label, frame != nullptr, &download)) save_image();

    // 說明列左邊：呼號、模式、第幾張、時間
    const float limit = video_button.x - S(12);
    if (frame != nullptr) {
        float x = rect.x + S(14);
        const std::string call = frame->callsign.empty() ? "—" : frame->callsign;
        text(dl, mono_bold_, 17, x, text_y, kText, call);
        x += text_width(mono_bold_, 17, call) + S(14);
        std::string info = LF("模式 %d", {frame->mode_id});
        if (const ModeInfo* mode = modes::find(frame->mode_id)) info = mode->label();
        if (frame->sequence >= 0) info += " · " + LF("第 %d 張", {frame->sequence + 1});
        info += " · " + schedule::time_text(frame->time);
        text(dl, regular_, 12, x, caption_y + (caption - S(12)) / 2 - S(1.5f), kSecondary, fitted(regular_, 12, info, limit - x));
    } else {
        text(dl, regular_, 12.5f, rect.x + S(14), caption_y + (caption - S(12.5f)) / 2 - S(1.5f), kSecondary, L("尚未收到畫面"));
    }
}

void UI::draw_signal_panel(const Rect& rect) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kPanel, S(10));
    const bool on_air = app_.is_on_air(), starting = app_.tx_phase == App::TxPhase::starting;
    const float row_h = rect.h / 5, left = rect.x + S(14), right = rect.right() - S(14);
    const auto row = [&](int index, const std::string& label, const std::string& value, bool mono, bool big = false, ImU32 color = kText) {
        const float y = rect.y + row_h * static_cast<float>(index);
        if (index > 0) dl->AddLine(ImVec2(left, y), ImVec2(right, y), kSeparator);
        text(dl, index == 0 ? bold_ : regular_, 13.0f, left, y + (row_h - S(13)) / 2 - S(1.5f), index == 0 ? kText : kSecondary, label);
        ImFont* font = big ? mono_bold_ : mono ? mono_ : regular_;
        const float size = big ? 17.0f : (mono ? 13.5f : 13.0f);
        text(dl, font, size, right - text_width(font, size, value), y + (row_h - S(size)) / 2 - S(1.5f), color, value);
    };
    const auto status = [&](const std::string& label, ImU32 color, bool filled) {
        const float y = rect.y + row_h / 2, w = text_width(regular_, 12, label);
        text(dl, regular_, 12, right - w, y - S(8), color, label);
        if (filled) dl->AddCircleFilled(ImVec2(right - w - S(9), y), S(3), color);
        else dl->AddCircle(ImVec2(right - w - S(9), y), S(3), color, 0, S(1.2f));
    };

    if (on_air || starting) {
        row(0, L("發射"), "", false);
        status(starting ? L("準備中") : app_.settings.ptt_mode == PTTMode::none ? L("發射中（VOX）") : L("PTT 已按下"), kTx, !starting);
        row(1, L("已發射"), App::clock_text(app_.tx_elapsed), true, true);
        row(2, L("已送出"), LF("%d 張", {app_.tx_frames_sent}), false);
        row(3, L("模式"), app_.tx_mode() != nullptr ? app_.tx_mode()->label() : std::string("—"), false);
        row(4, L("接收"), L("暫停，停止發射後自動恢復"), false, false, kSecondary);
        return;
    }
    row(0, L("訊號"), "", false);
    status(app_.synced ? L("已同步") : L("搜尋同步訊號中"), app_.synced ? kRx : kSecondary, app_.synced);
    row(1, L("訊雜比"), app_.snr_db ? App::db_text(*app_.snr_db) : std::string("—"), true, app_.snr_db.has_value());
    char offset[32];
    std::snprintf(offset, sizeof offset, "%+.0f Hz", app_.offset_hz.value_or(0));
    row(2, L("頻率偏移"), app_.offset_hz ? std::string(offset) : std::string("—"), true);
    std::string mode = "—";
    if (app_.rx_mode_id) {
        const ModeInfo* info = modes::find(*app_.rx_mode_id);
        mode = info != nullptr ? info->label() : LF("模式 %d", {*app_.rx_mode_id});
    }
    row(3, L("模式（自動偵測）"), mode, false);
    const std::string last = app_.last_result == App::LastResult::ok ? L("解碼成功") : app_.last_result == App::LastResult::failed ? L("解碼失敗")
                             : app_.last_result == App::LastResult::model_missing ? L("模型不符") : std::string("—");
    row(4, L("上一張"), last, false, false, app_.last_result == App::LastResult::failed || app_.last_result == App::LastResult::model_missing ? kTx : kText);
}

void UI::draw_recent_panel(const Rect& rect) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kPanel, S(10));
    text(dl, bold_, 13, rect.x + S(14), rect.y + S(13), kText, L("最近收到"));
    if (app_.history.size() > 6) {
        const std::string total = LF("共 %d 張，往下捲看更早的", {static_cast<int>(app_.history.size())});
        text(dl, regular_, 11, rect.right() - S(14) - text_width(regular_, 11, total), rect.y + S(15), kTertiary, total);
    }
    // 三欄的縮圖；格線佔的寬度是面板扣掉左右各 12，捲軸畫在右邊留白的地方，不擠到縮圖
    const Rect list{rect.x + S(12), rect.y + S(40), rect.w - S(24), rect.h - S(46)};
    if (app_.history.empty()) {
        dl->AddRect(ImVec2(list.x + 0.5f, list.y + 0.5f), ImVec2(list.right() - 0.5f, list.bottom() - 0.5f), kSeparator, S(7));
        const std::string hint = L("收到的畫面會依時間排在這裡");
        const float h = paragraph_height(regular_, 12, hint, list.w - S(24));
        paragraph(dl, regular_, 12, list.x + S(12), list.y + (list.h - h) / 2, list.w - S(24), kTertiary, hint, true);
        return;
    }
    ImGui::SetCursorScreenPos(ImVec2(list.x, list.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(7));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kButtonRing);
    ImGui::BeginChild("##recent", ImVec2(list.w + S(10), list.h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const int columns = 3;
    const float cell_gap = S(8);
    const float cell_w = (list.w - cell_gap * (columns - 1)) / columns, picture_h = cell_w * 3 / 4, cell_h = picture_h + S(21);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const int rows = (static_cast<int>(app_.history.size()) + columns - 1) / columns;
    const bool idle = app_.tx_phase == App::TxPhase::idle;   // 發射中主畫面顯示的是送出的那一張，不標「正在看的」
    // 只畫看得到的那幾列
    const float scroll = ImGui::GetScrollY();
    const int first_row = std::max(0, static_cast<int>(scroll / (cell_h + cell_gap)) - 1);
    const int last_row = std::min(rows, static_cast<int>((scroll + list.h) / (cell_h + cell_gap)) + 2);
    for (int r = first_row; r < last_row; ++r) {
        for (int c = 0; c < columns; ++c) {
            const std::size_t index = static_cast<std::size_t>(r * columns + c);
            if (index >= app_.history.size()) break;
            const App::ReceivedFrame& frame = app_.history[index];
            const Rect cell{origin.x + (cell_w + cell_gap) * static_cast<float>(c), origin.y + (cell_h + cell_gap) * static_cast<float>(r), cell_w, picture_h};
            note_hit("##thumb/" + std::to_string(index), Rect{cell.x, cell.y, cell.w, cell_h});
            ImGui::SetCursorScreenPos(ImVec2(cell.x, cell.y));
            ImGui::PushID(static_cast<int>(frame.id & 0x7FFFFFFF));
            if (ImGui::InvisibleButton("##thumb", ImVec2(cell.w, cell_h))) app_.select(frame);
            ImGui::PopID();
            picture(cdl, frame.picture, cell, S(4));
            if (idle && app_.is_displayed(frame)) cdl->AddRect(ImVec2(cell.x + S(1), cell.y + S(1)), ImVec2(cell.right() - S(1), cell.bottom() - S(1)), kRx, S(4), S(2));
            if (app_.settings.show_frame_snr) draw_snr_badge(cdl, cell.x + S(3), cell.y + S(3), frame.snr_db, 9);
            // 時間在左、模式在右；格子太窄放不下兩樣時只留時間
            const std::string time = schedule::time_text(frame.time), mode = LF("模式 %d", {frame.mode_id});
            const float time_w = text_width(mono_, 10.5f, time), mode_w = text_width(regular_, 10.5f, mode);
            text(cdl, mono_, 10.5f, cell.x, cell.bottom() + S(5), kSecondary, time);
            if (time_w + mode_w + S(4) <= cell.w) text(cdl, regular_, 10.5f, cell.right() - mode_w, cell.bottom() + S(4.5f), kTertiary, mode);
        }
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(list.w, static_cast<float>(rows) * (cell_h + cell_gap) - cell_gap));
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void UI::draw_spectrum(const Rect& rect) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool on_air = app_.is_on_air();
    // 標題列
    const std::string title = L("音訊頻譜 0–3 kHz"), hint = on_air ? L("發射中顯示送出的訊號") : L("白線之間是訊號頻帶 352–2648 Hz");
    text(dl, bold_, 12, rect.x, rect.y, kText, title);
    text(dl, regular_, 12, rect.x + text_width(bold_, 12, title) + S(12), rect.y, kTertiary, hint);
    const std::string meter_label = on_air ? L("輸出音量") : L("輸入音量");
    const std::string meter_value = App::db_text(on_air ? app_.settings.output_level_db : std::max(app_.input_level_db, -99.0));
    const float value_w = text_width(mono_, 12, "−99 dB");
    const Rect bar{rect.right() - value_w - S(10) - S(120), rect.y + S(5), S(120), S(5)};
    text(dl, mono_, 12, rect.right() - text_width(mono_, 12, meter_value), rect.y, kText, meter_value);
    level_bar(dl, bar, on_air ? App::level_fraction(app_.settings.output_level_db, -40) : App::level_fraction(app_.input_level_db), on_air ? kTx : kRx);
    text(dl, regular_, 12, bar.x - S(10) - text_width(regular_, 12, meter_label), rect.y, kSecondary, meter_label);

    // 瀑布圖：最新的在上面；顯示的高度相當於圖上的幾列，就取那幾列
    const float axis = S(22);
    const Rect fall{rect.x, rect.y + S(26), rect.w, std::max(S(40), rect.h - S(26) - axis)};
    dl->AddRectFilled(ImVec2(fall.x, fall.y), ImVec2(fall.right(), fall.bottom()), kWaterfall, S(8));
    if (waterfall_texture_ != nullptr) {
        const float lines = std::min(static_cast<float>(WaterfallStore::kHeight), fall.h / scale_);   // 圖上的一列畫成一個點高
        dl->AddImageRounded(tex(waterfall_texture_), ImVec2(fall.x, fall.y), ImVec2(fall.right(), fall.bottom()), ImVec2(0, 0), ImVec2(1, lines / WaterfallStore::kHeight), IM_COL32_WHITE, S(8));
    }
    const auto x_of = [&](double hz) { return fall.x + fall.w * static_cast<float>(hz / kSpectrumMaxHz); };
    for (const double edge : {kBandLowHz, kBandHighHz}) dl->AddLine(ImVec2(x_of(edge), fall.y), ImVec2(x_of(edge), fall.bottom()), rgb(0xFFFFFF, 190));
    for (int hz = 0; hz <= 3000; hz += 500) {
        const std::string label = hz == 3000 ? "3000 Hz" : std::to_string(hz);
        const float w = text_width(mono_, 10.5f, label);
        const float x = hz == 0 ? fall.x : hz == 3000 ? fall.right() - w : x_of(hz) - w / 2;
        text(dl, mono_, 10.5f, x, fall.bottom() + S(6), kTertiary, label);
    }
}

// ---------------------------------------------------------------- 右側欄

void UI::draw_side_panel(const Rect& rect) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(rect.x, rect.y), ImVec2(rect.right(), rect.bottom()), kAside);
    dl->AddLine(ImVec2(rect.x + 0.5f, rect.y), ImVec2(rect.x + 0.5f, rect.bottom()), kBarBorder);
    const float left = rect.x + S(16), width = rect.w - S(32);
    const bool busy = app_.tx_phase != App::TxPhase::idle;
    Settings& settings = app_.settings;
    const bool is_video = settings.source_kind == SourceKind::video && app_.video_name && !app_.video_is_image;
    const bool is_images = settings.source_kind == SourceKind::images && !app_.image_files.empty();

    // 先把會折行的兩段字準備好：它們有多高，決定預覽可以多大
    const ModeInfo* mode = app_.tx_mode();
    const std::string mode_label = mode != nullptr ? mode->label() : L("沒有可以用的模式");
    const std::string summary = busy ? L("發射中不能更換模式；來源可以隨時切換，下一張生效") : mode != nullptr ? mode->summary() : L("app 裡找不到模型的權重檔");
    const float summary_h = paragraph_height(regular_, 12, summary, width);
    std::string hint;
    if (app_.tx_phase == App::TxPhase::stopping) hint = L("正在送完這張；再按一次立刻停止（這張不完整）");
    else if (busy && app_.scheduled_run) hint = L("定時發射：影片播完會自動停止，也可以隨時按停止");
    else if (busy && app_.video_plays_once()) hint = L("影片播完會自動停止，也可以隨時按停止");
    else if (busy && app_.images_play_once()) hint = L("圖片送完會自動停止，也可以隨時按停止");
    else if (busy) hint = L("送完目前這張後放開 PTT，自動回到接收");
    else if (settings.callsign.empty()) hint = L("還沒設定呼號，請先到設定裡填寫");
    else if (!app_.can_transmit()) hint = L("等來源有畫面後才能發射");
    else if (app_.video_plays_once()) hint = LF("以 %@ 發射，影片播完自動停止", {settings.callsign});
    else if (app_.images_play_once()) hint = LF("以 %@ 發射，圖片送完自動停止", {settings.callsign});
    else hint = LF("以 %@ 發射，停止後自動回到接收", {settings.callsign});
    const float hint_h = paragraph_height(regular_, 11.5f, hint, width);
    // 底下那一塊（輸出音量、發射鈕、說明）與預覽以外的每一列加起來要多高
    const float bottom_block = S(22) + S(14) + S(44) + S(10) + hint_h + S(16);
    const float below_preview = S(8) + (is_video || is_images ? S(34) : 0) + S(26) + S(38) + S(42) + S(14) + S(24) + S(40) + summary_h + S(14);
    float y = rect.y + S(18);

    // ---- 發射來源
    text(dl, bold_, 12, left, y, kSecondary, L("發射來源"));
    y += S(24);
    const int current_source = settings.source_kind == SourceKind::camera ? 0 : settings.source_kind == SourceKind::video ? 1 : 2;
    const int source = segmented("##source", Rect{left, y, width, S(30)}, {L("鏡頭"), L("影片檔"), L("圖片序列")}, current_source);
    if (source != current_source) app_.set_source_kind(source == 0 ? SourceKind::camera : source == 1 ? SourceKind::video : SourceKind::images);
    y += S(40);
    if (settings.source_kind == SourceKind::camera) {
        Icon camera = Icon::camera;
        const std::string name = app_.cameras.empty() ? L("（沒有鏡頭）") : app_.camera_name();
        if (popup_button("##camera", Rect{left, y, width, S(30)}, name, !app_.cameras.empty(), &camera)) ImGui::OpenPopup("##cameramenu");
        tooltip(LF("選擇鏡頭：%@", {name}));
        ImGui::SetNextWindowPos(ImVec2(left, y + S(34)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenu);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kMenuSelected);
        ImGui::PushStyleColor(ImGuiCol_Header, kMenuSelected);
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        if (ImGui::BeginPopup("##cameramenu")) {
            close_popup_on_escape();
            ImGui::PushFont(regular_, 12.5f * scale_);
            for (const CameraInfo& info : app_.cameras) {
                if (ImGui::Selectable(info.name.c_str(), info.name == name, ImGuiSelectableFlags_None, ImVec2(width - S(12), 0))) app_.select_camera(info.name);
            }
            ImGui::PopFont();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
    } else if (settings.source_kind == SourceKind::video) {
        const std::string choose = L("選擇…");
        const float choose_w = text_width(regular_, 12.5f, choose) + S(24);
        const Rect name_box{left, y, width - choose_w - S(8), S(30)};
        dl->AddRectFilled(ImVec2(name_box.x, name_box.y), ImVec2(name_box.right(), name_box.bottom()), kWell, S(7));
        dl->AddRect(ImVec2(name_box.x + 0.5f, name_box.y + 0.5f), ImVec2(name_box.right() - 0.5f, name_box.bottom() - 0.5f), kSeparator, S(7));
        icon(dl, Icon::video_file, name_box.x + S(9), name_box.y + S(8), S(14), S(14), kSecondary);
        const std::string name = app_.video_name ? *app_.video_name : L("尚未選擇影片檔");
        text(dl, regular_, 12.5f, name_box.x + S(30), name_box.y + S(7), app_.video_name ? kText : kTertiary, fitted(regular_, 12.5f, name, name_box.w - S(40)));
        if (button("##choosevideo", Rect{name_box.right() + S(8), y, choose_w, S(30)}, choose)) choose_video_file();
    } else {
        // 圖片序列：框裡寫的是「下一張要送的」那個圖片檔（就是下面預覽的那一張）；「選擇…」換一批圖片
        const std::string choose = L("選擇…");
        const float choose_w = text_width(regular_, 12.5f, choose) + S(24);
        const Rect name_box{left, y, width - choose_w - S(8), S(30)};
        dl->AddRectFilled(ImVec2(name_box.x, name_box.y), ImVec2(name_box.right(), name_box.bottom()), kWell, S(7));
        dl->AddRect(ImVec2(name_box.x + 0.5f, name_box.y + 0.5f), ImVec2(name_box.right() - 0.5f, name_box.bottom() - 0.5f), kSeparator, S(7));
        icon(dl, Icon::image_file, name_box.x + S(9), name_box.y + S(8), S(14), S(14), kSecondary);
        const std::optional<std::string> next = app_.next_image();
        const std::string name = next ? file_name(*next) : L("尚未選擇圖片");
        text(dl, regular_, 12.5f, name_box.x + S(30), name_box.y + S(7), next ? kText : kTertiary, fitted(regular_, 12.5f, name, name_box.w - S(40)));
        ImGui::SetCursorScreenPos(ImVec2(name_box.x, name_box.y));
        ImGui::Dummy(ImVec2(name_box.w, name_box.h));
        tooltip(next ? LF("下一張要送的圖片：%@", {*next}) : L("選擇要依序發射的圖片，或選一個資料夾"));
        // 系統的開檔對話框一次只能選檔案或選資料夾，所以「選擇…」先問是哪一種
        const Rect choose_rect{name_box.right() + S(8), y, choose_w, S(30)};
        if (button("##chooseimages", choose_rect, choose)) ImGui::OpenPopup("##imagesmenu");
        tooltip(L("選擇要依序發射的圖片（可以多選），或選資料夾：裡面的圖片全部加入。順序照檔名"));
        const std::string pick_files = L("選擇圖片檔…"), pick_folder = L("選擇資料夾…");
        const float menu_w = std::max(text_width(regular_, 12.5f, pick_files), text_width(regular_, 12.5f, pick_folder)) + S(28);
        ImGui::SetNextWindowPos(ImVec2(choose_rect.right() - menu_w - S(12), y + S(34)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(5)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenu);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kMenuSelected);
        ImGui::PushStyleColor(ImGuiCol_Header, kMenuSelected);
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        if (ImGui::BeginPopup("##imagesmenu")) {
            close_popup_on_escape();
            ImGui::PushFont(regular_, 12.5f * scale_);
            const auto item = [&](const char* id, const std::string& label) {
                const bool chosen = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(menu_w, 0));
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                note_hit(id, Rect{a.x, a.y, b.x - a.x, b.y - a.y});
                return chosen;
            };
            if (item("##imagesmenu/files", pick_files)) choose_images();
            if (item("##imagesmenu/folder", pick_folder)) choose_image_folder();
            ImGui::PopFont();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
    }
    y += S(40);

    // ---- 預覽：縮成 96×72、印好畫布的字，就是會送出去的樣子。平常和側欄一樣寬；視窗矮的時候縮小，讓底下的東西都放得下
    const float preview_h = std::clamp(rect.bottom() - bottom_block - below_preview - y, std::min(S(90), width * 3 / 4), width * 3 / 4);
    const Rect preview{left + (width - preview_h * 4 / 3) / 2, y, preview_h * 4 / 3, preview_h};
    if (app_.source_preview) {
        picture(dl, app_.source_preview, preview, S(6));
    } else {
        dl->AddRectFilled(ImVec2(preview.x, preview.y), ImVec2(preview.right(), preview.bottom()), kPlaceholder, S(6));
        const std::string message = app_.source_message ? *app_.source_message : L("等待來源畫面");
        const float wrap = preview.w - S(32);
        paragraph(dl, regular_, 12, preview.x + S(16), preview.y + (preview.h - paragraph_height(regular_, 12, message, wrap)) / 2, wrap, kTertiary, message, true);
    }
    y += preview.h + S(8);

    // ---- 影片的播放列
    if (is_video) {
        const Rect play{left, y, S(26), S(26)};
        note_hit("##play", play);
        ImGui::SetCursorScreenPos(ImVec2(play.x, play.y));
        if (ImGui::InvisibleButton("##play", ImVec2(play.w, play.h))) app_.toggle_video_playback();
        const bool hovered = ImGui::IsItemHovered();
        tooltip(app_.video_playing ? L("暫停") : L("播放"));
        dl->AddRectFilled(ImVec2(play.x, play.y), ImVec2(play.right(), play.bottom()), hovered ? kButtonHover : kButton, S(6));
        dl->AddRect(ImVec2(play.x + 0.5f, play.y + 0.5f), ImVec2(play.right() - 0.5f, play.bottom() - 0.5f), kButtonRing, S(6));
        icon(dl, app_.video_playing ? Icon::pause : Icon::play, play.x + S(7.5f), play.y + S(7.5f), S(11), S(11), kText);
        const std::string time = App::clock_text(app_.video_position) + " / " + App::clock_text(app_.video_duration);
        const float time_w = text_width(mono_, 11.5f, time);
        double position = app_.video_position;
        const Rect bar{play.right() + S(12), y, width - S(26) - S(12) - S(10) - time_w, S(26)};
        const bool moved = slider("##scrub", bar, position, 0, std::max(app_.video_duration, 0.1), kRx);
        const bool dragging = ImGui::IsItemActive();
        tooltip(L("播放位置"));
        if (moved) app_.scrub_video(position, true);
        else if (!dragging && ImGui::IsItemDeactivated()) app_.scrub_video(position, false);
        text(dl, mono_, 11.5f, rect.right() - S(16) - time_w, y + S(6), kSecondary, time);
        y += S(34);
    }
    // ---- 圖片序列的位置：上一張、下一張、拖到第幾張，以及「第幾張 / 共幾張」。指的都是「下一張要送的」那一張
    if (is_images) {
        const int count = static_cast<int>(app_.image_files.size()), index = app_.image_index;
        const auto step = [&](const char* id, Icon kind, float x, bool enabled, const std::string& title, int target) {
            const Rect box{x, y, S(26), S(26)};
            note_hit(id, box);
            ImGui::SetCursorScreenPos(ImVec2(box.x, box.y));
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(box.w, box.h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::EndDisabled();
            tooltip(title);
            dl->AddRectFilled(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), enabled ? (hovered ? kButtonHover : kButton) : kDisabledFill, S(6));
            dl->AddRect(ImVec2(box.x + 0.5f, box.y + 0.5f), ImVec2(box.right() - 0.5f, box.bottom() - 0.5f), enabled ? kButtonRing : kSeparator, S(6));
            icon(dl, kind, box.x + S(7.5f), box.y + S(7.5f), S(11), S(11), enabled ? kText : kTertiary, 1.6f);
            if (clicked && enabled) app_.select_image(target);
        };
        step("##previmage", Icon::previous, left, index > 0, L("上一張圖片"), index - 1);
        step("##nextimage", Icon::next, left + S(30), index < count - 1, L("下一張圖片"), index + 1);
        const std::string place = std::to_string(index + 1) + " / " + std::to_string(count);
        const float place_w = text_width(mono_, 11.5f, place);
        if (count > 1) {
            double position = index + 1;
            const float bar_x = left + S(56) + S(12);
            const Rect bar{bar_x, y, rect.right() - S(16) - place_w - S(10) - bar_x, S(26)};
            if (slider("##imageposition", bar, position, 1, count, kRx)) app_.select_image(static_cast<int>(std::lround(position)) - 1);
            tooltip(L("下一張要送第幾張"));
        }
        text(dl, mono_, 11.5f, rect.right() - S(16) - place_w, y + S(6), kSecondary, place);
        y += S(34);
    }
    const std::string size_note = app_.source_size ? LF("來源 %d×%d → 送出 96×72", {app_.source_size->first, app_.source_size->second}) : L("來源 — → 送出 96×72");
    text(dl, regular_, 11.5f, left, y, kTertiary, fitted(regular_, 11.5f, size_note, width));
    y += S(26);

    // ---- 循環播放、縮放方式
    const std::string fill = L("填滿"), fit = L("完整");
    const float scale_w = text_width(bold_, 12.5f, fill) + text_width(bold_, 12.5f, fit) + S(44);
    const Rect scale_rect{rect.right() - S(16) - scale_w, y, scale_w, S(28)};
    if (is_video) {
        if (checkbox("##loop", left, y + S(5), fitted(regular_, 12.5f, L("循環播放"), scale_rect.x - left - S(34)), settings.loop_video)) settings.loop_video = !settings.loop_video;
        tooltip(L("沒有勾：按下發射才開始播放，播完自動停止發射。勾了：影片循環播放，要自己按停止。兩種都是每送一張取當下那一格"));
    } else if (is_images) {
        // 圖片序列的「循環播放」和影片檔的是分開的兩個設定
        if (checkbox("##loopimages", left, y + S(5), fitted(regular_, 12.5f, L("循環播放"), scale_rect.x - left - S(34)), settings.loop_images)) settings.loop_images = !settings.loop_images;
        tooltip(L("沒有勾：從預覽的這一張送到最後一張，每張送一次，送完自動停止發射。勾了：送完最後一張再從第一張開始，要自己按停止"));
    } else {
        text(dl, regular_, 12.5f, left, y + S(6), kSecondary, fitted(regular_, 12.5f, L("縮放方式"), scale_rect.x - left - S(8)));
    }
    const int scale_mode = segmented("##scale", scale_rect, {fill, fit}, settings.scale_mode == ScaleMode::fill ? 0 : 1);
    settings.scale_mode = scale_mode == 0 ? ScaleMode::fill : ScaleMode::fit;
    tooltip(L("縮放方式。填滿：把畫面放大到蓋滿，裁掉多出來的邊。完整：整個畫面都留著，上下或左右補黑邊"));
    y += S(38);

    // ---- 畫布
    const std::string canvas = L("畫布"), top_label = L("上"), bottom_label = L("下");
    if (checkbox("##canvas", left, y + S(6), canvas, settings.canvas_enabled)) app_.set_canvas_enabled(!settings.canvas_enabled);
    tooltip(LF("在送出去的每一張畫面上印字，例如「CQ %@」。上面的預覽就是送出去的樣子", {settings.callsign.empty() ? L("呼號") : settings.callsign}));
    const float canvas_x = left + S(15) + S(8) + text_width(regular_, 12.5f, canvas) + S(10);
    const float position_w = std::max(S(62), text_width(bold_, 12.5f, top_label) + text_width(bold_, 12.5f, bottom_label) + S(36));
    const Rect position_rect{rect.right() - S(16) - position_w, y, position_w, S(30)};
    const Rect canvas_field{canvas_x, y, position_rect.x - S(8) - canvas_x, S(30)};
    if (canvas_buffer_ != settings.canvas_text && !ImGui::IsAnyItemActive()) canvas_buffer_ = settings.canvas_text;
    if (input_text("##canvastext", canvas_field, canvas_buffer_, L("要印的字"), false, false, 120)) settings.canvas_text = canvas_buffer_;
    const int position = segmented("##canvaspos", position_rect, {top_label, bottom_label}, settings.canvas_position == CanvasPosition::top ? 0 : 1);
    settings.canvas_position = position == 0 ? CanvasPosition::top : CanvasPosition::bottom;
    y += S(42);
    dl->AddLine(ImVec2(left, y), ImVec2(left + width, y), kSeparator);
    y += S(14);

    // ---- 發射模式
    text(dl, bold_, 12, left, y, kSecondary, L("發射模式"));
    y += S(24);
    mode_anchor_ = Rect{left, y, width, S(32)};
    if (popup_button("##mode", mode_anchor_, mode_label, mode != nullptr, nullptr, busy)) want_mode_menu_ = true;
    tooltip(busy ? LF("發射模式：%@，發射中不能更換", {mode_label}) : LF("選擇發射模式：%@", {mode_label}));
    y += S(40);
    paragraph(dl, regular_, 12, left, y, width, kTertiary, summary);

    // ---- 從底下往上排：說明、發射鈕、輸出音量
    float by = rect.bottom() - S(16) - hint_h;
    paragraph(dl, regular_, 11.5f, left, by, width, kTertiary, hint, true);
    by -= S(10) + S(44);
    const Rect tx_button{left, by, width, S(44)};
    note_hit("##transmit", tx_button);
    const bool can_start = app_.can_transmit() && !settings.callsign.empty();
    const bool enabled = busy || can_start;
    ImGui::SetCursorScreenPos(ImVec2(tx_button.x, tx_button.y));
    ImGui::BeginDisabled(!enabled);
    if (ImGui::InvisibleButton("##transmit", ImVec2(tx_button.w, tx_button.h))) app_.toggle_transmit();
    const bool tx_hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    const std::string tx_label = app_.tx_phase == App::TxPhase::idle ? L("開始發射") : app_.tx_phase == App::TxPhase::stopping ? L("這張送完就停止…") : L("停止發射");
    if (busy) {
        dl->AddRectFilled(ImVec2(tx_button.x, tx_button.y), ImVec2(tx_button.right(), tx_button.bottom()), tx_hovered ? rgb(0x2B2118) : kAside, S(9));
        dl->AddRect(ImVec2(tx_button.x + 1, tx_button.y + 1), ImVec2(tx_button.right() - 1, tx_button.bottom() - 1), kTx, S(9), S(2));
    } else {
        dl->AddRectFilled(ImVec2(tx_button.x, tx_button.y), ImVec2(tx_button.right(), tx_button.bottom()), enabled ? (tx_hovered ? rgb(0xFFB888) : kTx) : kDisabledFill, S(9));
    }
    const std::string tx_shown = fitted(bold_, 15, tx_label, tx_button.w - S(16));
    text(dl, bold_, 15, tx_button.x + (tx_button.w - text_width(bold_, 15, tx_shown)) / 2, tx_button.y + (tx_button.h - S(15)) / 2 - S(2), busy ? kTx : enabled ? kTxInk : kTertiary, tx_shown);
    by -= S(14) + S(22);
    const std::string level_label = L("輸出音量"), level_value = App::db_text(settings.output_level_db);
    text(dl, regular_, 12.5f, left, by + S(3), kSecondary, level_label);
    const float level_value_w = text_width(mono_, 12, "−40 dB");
    text(dl, mono_, 12, rect.right() - S(16) - text_width(mono_, 12, level_value), by + S(3), kText, level_value);
    const float slider_x = left + text_width(regular_, 12.5f, level_label) + S(12);
    double level = settings.output_level_db;
    if (slider("##level", Rect{slider_x, by, rect.right() - S(16) - level_value_w - S(10) - slider_x, S(22)}, level, -40, 0, kTx)) settings.output_level_db = std::round(level);
    tooltip(L("送去電台的聲音大小。發射中看著電台的 ALC 調：ALC 剛好不動或只動一點點"));
}

// ---------------------------------------------------------------- 浮在上面的面板

void UI::draw_mode_menu() {
    if (want_mode_menu_) {
        ImGui::OpenPopup("##modemenu");
        want_mode_menu_ = false;
    }
    const std::vector<ModeInfo> list = modes::menu_order();
    const float w = std::max(S(400), mode_anchor_.w + S(90));
    ImGui::SetNextWindowPos(ImVec2(mode_anchor_.right() - w, mode_anchor_.bottom() + S(6)));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(12)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(11));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenu);
    ImGui::PushStyleColor(ImGuiCol_Border, kButtonRing);
    if (ImGui::BeginPopup("##modemenu")) {
        close_popup_on_escape();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float inner = w - S(20);
        const float col_name = origin.x + S(34), col_quality = origin.x + S(110), col_time = origin.x + inner - S(120), col_content = origin.x + inner - S(10);
        float y = origin.y;
        text(dl, regular_, 11.5f, col_name, y, kTertiary, L("模式"));
        text(dl, regular_, 11.5f, col_time - text_width(regular_, 11.5f, L("每張")), y, kTertiary, L("每張"));
        text(dl, regular_, 11.5f, col_content - text_width(regular_, 11.5f, L("內容")), y, kTertiary, L("內容"));
        y += S(24);
        if (list.empty()) {
            text(dl, regular_, 12.5f, origin.x + S(12), y, kSecondary, L("沒有可以用的模式"));
            y += S(22);
            text(dl, regular_, 11.5f, origin.x + S(12), y, kTertiary, L("app 裡找不到模型的權重檔"));
            y += S(24);
        } else {
            text(dl, bold_, 12, origin.x + S(12), y, kSecondary, L("類神經網路編碼（實驗中）"));
            y += S(19);
            y += paragraph(dl, regular_, 11.5f, origin.x + S(12), y, inner - S(24), kTertiary, L("訊號愈弱畫面愈糊，不會整張收不到；兩邊要用同一份權重")) + S(8);
            for (const ModeInfo& mode : list) {
                const Rect row{origin.x, y, inner, S(30)};
                note_hit("##moderow/" + std::to_string(mode.id), row);
                ImGui::SetCursorScreenPos(ImVec2(row.x, row.y));
                ImGui::PushID(mode.id);
                const bool clicked = ImGui::InvisibleButton("##moderow", ImVec2(row.w, row.h));
                ImGui::PopID();
                const bool selected = mode.id == app_.settings.tx_mode_id;
                if (selected || ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(row.x, row.y), ImVec2(row.right(), row.bottom()), kMenuSelected, S(7));
                if (selected) icon(dl, Icon::check, row.x + S(10), row.y + S(8), S(14), S(14), kRx, 2.0f);
                const float ty = row.y + S(7);
                text(dl, bold_, 13, col_name, ty, kText, mode.name());
                text(dl, regular_, 12.5f, col_quality, ty, kSecondary, mode.quality());
                const std::string seconds = mode.seconds_text(), content = mode.content_text();
                text(dl, mono_, 13, col_time - text_width(mono_, 13, seconds), ty, kText, seconds);
                text(dl, regular_, 12.5f, col_content - text_width(regular_, 12.5f, content), ty, kSecondary, content);
                if (clicked) {
                    app_.settings.tx_mode_id = mode.id;
                    ImGui::CloseCurrentPopup();
                }
                y += S(32);
            }
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(inner, y - origin.y));
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void UI::draw_schedule_menu() {
    if (want_schedule_menu_) {
        ImGui::OpenPopup("##schedulemenu");
        want_schedule_menu_ = false;
    }
    Settings& settings = app_.settings;
    const float w = S(372);
    ImGui::SetNextWindowPos(ImVec2(schedule_anchor_.right() - w, schedule_anchor_.bottom() + S(8)));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(16)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(11));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenu);
    ImGui::PushStyleColor(ImGuiCol_Border, kButtonRing);
    if (ImGui::BeginPopup("##schedulemenu")) {
        close_popup_on_escape();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float inner = w - S(32), left = origin.x;
        float y = origin.y;
        text(dl, bold_, 14, left, y, kText, L("定時發射"));
        y += S(22);
        y += paragraph(dl, regular_, 12, left, y, inner, kSecondary, L("到了時間自動把影片檔從頭發射一次，播完就停止")) + S(12);
        if (checkbox("##armed", left, y, L("開啟定時發射"), app_.schedule_armed)) app_.set_schedule_armed(!app_.schedule_armed);
        y += S(30);
        text(dl, regular_, 12, left, y, kSecondary, L("每隔幾分鐘"));
        y += S(22);
        std::vector<std::string> labels;
        int selected = 0;
        for (std::size_t i = 0; i < schedule::intervals().size(); ++i) {
            labels.push_back(std::to_string(schedule::intervals()[i]));
            if (schedule::intervals()[i] == settings.schedule_interval_minutes) selected = static_cast<int>(i);
        }
        const int chosen = segmented("##interval", Rect{left, y, inner, S(30)}, labels, selected);
        if (chosen != selected) {
            settings.schedule_interval_minutes = schedule::intervals()[static_cast<std::size_t>(chosen)];
            app_.schedule_interval_changed();
        }
        y += S(40);
        y += paragraph(dl, regular_, 12, left, y, inner, kSecondary, LF("每小時的 %@ 分整", {schedule::minutes_text(settings.schedule_interval_minutes)})) + S(10);
        text(dl, regular_, 12, left, y, kSecondary, L("要播的影片"));
        y += S(22);
        const std::string choose = L("選擇…");
        const float choose_w = text_width(regular_, 12.5f, choose) + S(24);
        const Rect name_box{left, y, inner - choose_w - S(8), S(30)};
        dl->AddRectFilled(ImVec2(name_box.x, name_box.y), ImVec2(name_box.right(), name_box.bottom()), kWell, S(7));
        dl->AddRect(ImVec2(name_box.x + 0.5f, name_box.y + 0.5f), ImVec2(name_box.right() - 0.5f, name_box.bottom() - 0.5f), kSeparator, S(7));
        icon(dl, Icon::video_file, name_box.x + S(9), name_box.y + S(8), S(14), S(14), kSecondary);
        const std::string duration = app_.video_name && !app_.video_is_image ? App::clock_text(app_.video_duration) : std::string();
        const float duration_w = duration.empty() ? 0 : text_width(mono_, 11.5f, duration) + S(10);
        text(dl, regular_, 12.5f, name_box.x + S(30), name_box.y + S(7), app_.video_name ? kText : kTertiary,
             fitted(regular_, 12.5f, app_.video_name ? *app_.video_name : L("尚未選擇影片檔"), name_box.w - S(40) - duration_w));
        if (!duration.empty()) text(dl, mono_, 11.5f, name_box.right() - duration_w, name_box.y + S(8), kTertiary, duration);
        if (button("##schedulevideo", Rect{name_box.right() + S(8), y, choose_w, S(30)}, choose)) choose_video_file();
        y += S(40);
        if (app_.video_is_image) {
            y += paragraph(dl, regular_, 11.5f, left, y, inner, kTertiary, LF("選的是圖片：每次連續送 %d 秒", {static_cast<int>(App::still_schedule_seconds)})) + S(6);
        }
        if (checkbox("##busy", left, y, L("正在收別人的畫面時，那一次不發射"), settings.schedule_skip_when_busy)) settings.schedule_skip_when_busy = !settings.schedule_skip_when_busy;
        y += S(32);
        dl->AddLine(ImVec2(left, y), ImVec2(left + inner, y), kSeparator);
        y += S(14);
        std::string state;
        if (app_.scheduled_run) state = L("正在發射這一次的影片");
        else if (!app_.schedule_armed) state = L("目前關著，不會自己發射");
        else if (const std::optional<std::string> problem = app_.schedule_problem()) state = LF("%@，到了時間不會發射", {*problem});
        else if (app_.schedule_next_fire) state = LF("下一次 %@", {schedule::clock_text(*app_.schedule_next_fire)});
        y += paragraph(dl, bold_, 12.5f, left, y, inner, kText, state) + S(4);
        if (app_.schedule_last_event) {
            y += paragraph(dl, regular_, 12, left, y, inner, kSecondary, LF("上一次：%@", {*app_.schedule_last_event})) + S(6);
        }
        for (const std::string& note : {L("用當時選的發射模式；來源是鏡頭的話，播完會換回鏡頭。正在發射時到了時間，那一次就跳過。"),
                                             L("開著的時候會請系統不要因為閒置而睡眠；重新開啟 app 之後要再打開一次。")}) {
            y += paragraph(dl, regular_, 11.5f, left, y, inner, kTertiary, note) + S(4);
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(inner, y - origin.y));
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

/// 按了「儲存影片」跳出來的一小塊：存檔之前的選項「在每一張的左上角印上訊雜比」，按「儲存…」才開系統的存檔對話框。
/// macOS 版把這個選項放在系統的存檔對話框裡；這裡的存檔對話框也是系統畫的，但 SDL 沒有辦法往裡面加東西，所以放在它前面一步。
/// 勾不勾當下就記進設定，下一次存影片時沿用。
void UI::draw_save_video_menu() {
    if (want_save_video_menu_) {
        ImGui::OpenPopup("##savevideomenu");
        want_save_video_menu_ = false;
    }
    Settings& settings = app_.settings;
    const std::string option = L("在每一張的左上角印上訊雜比"), go = L("儲存…");
    const float go_w = text_width(regular_, 12.5f, go) + S(32);
    const float inner = std::max(S(15) + S(8) + text_width(regular_, 12.5f, option), go_w), w = inner + S(28);
    ImGui::SetNextWindowPos(ImVec2(std::max(S(8), save_video_anchor_.right() - w), save_video_anchor_.bottom() + S(6)));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(11));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenu);
    ImGui::PushStyleColor(ImGuiCol_Border, kButtonRing);
    if (ImGui::BeginPopup("##savevideomenu")) {
        close_popup_on_escape();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (checkbox("##stampsnr", origin.x, origin.y, option, settings.stamp_video_snr)) settings.stamp_video_snr = !settings.stamp_video_snr;
        // 這段時間裡那一段可能變了（又收到新的畫面、正在存上一個）：存不了的時候按鈕是灰的
        const bool can_save = app_.displayed_sequence().size() >= 2 && !app_.exporting_video && FileSource::ffmpeg_available();
        const Rect go_rect{origin.x + inner - go_w, origin.y + S(18) + S(12), go_w, S(30)};
        if (button("##savevideogo", go_rect, go, can_save)) {
            ImGui::CloseCurrentPopup();
            save_video();
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(inner, S(18) + S(12) + S(30)));
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void UI::draw_settings() {
    ImGuiIO& io = ImGui::GetIO();
    Settings& settings = app_.settings;
    // 設定是一個蓋在主視窗上面的對話框：開著的時候底下的東西按不到、顏色壓暗；按到外面或按 Esc 就關掉
    if (show_settings && !ImGui::IsPopupOpen("##settingsdialog")) ImGui::OpenPopup("##settingsdialog");
    if (!show_settings && !ImGui::IsPopupOpen("##settingsdialog")) return;
    const float w = std::min(S(580), io.DisplaySize.x - S(40)), h = std::min(S(866), io.DisplaySize.y - S(24));
    const ImVec2 corner((io.DisplaySize.x - w) / 2, (io.DisplaySize.y - h) / 2);
    ImGui::SetNextWindowPos(corner);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(9));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, kButtonRing);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kButtonRing);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, rgb(0x000000, 130));
    const bool open = ImGui::BeginPopupModal("##settingsdialog", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(4);
    if (!open) return;
    // 焦點在這個對話框上（沒有開著它裡面的選單）時：按到外面、或沒有在打字時按 Esc，就關掉
    if (ImGui::IsWindowFocused()) {
        const ImVec2 m = io.MousePos;
        const bool outside = m.x < corner.x || m.x > corner.x + w || m.y < corner.y || m.y > corner.y + h;
        if ((outside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())) show_settings = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float left = origin.x + S(24), inner = w - S(48);
    float y = origin.y;

    // 標題列
    dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + w, y + S(44)), kBar, S(12), ImDrawFlags_RoundTop);
    dl->AddLine(ImVec2(origin.x, y + S(44)), ImVec2(origin.x + w, y + S(44)), kBarBorder);
    const std::string title = L("設定");
    text(dl, bold_, 13.5f, origin.x + (w - text_width(bold_, 13.5f, title)) / 2, y + S(13), kText, title);
    const Rect close{origin.x + w - S(40), y + S(8), S(28), S(28)};
    note_hit("##closesettings", close);
    ImGui::SetCursorScreenPos(ImVec2(close.x, close.y));
    if (ImGui::InvisibleButton("##closesettings", ImVec2(close.w, close.h))) show_settings = false;
    const ImU32 cross = ImGui::IsItemHovered() ? kText : kSecondary;
    dl->AddLine(ImVec2(close.x + S(9), close.y + S(9)), ImVec2(close.right() - S(9), close.bottom() - S(9)), cross, S(1.6f));
    dl->AddLine(ImVec2(close.right() - S(9), close.y + S(9)), ImVec2(close.x + S(9), close.bottom() - S(9)), cross, S(1.6f));
    y += S(44) + S(18);

    const float row_h = S(54), field_y = (row_h - S(32)) / 2;
    const auto section = [&](const std::string& name) {
        text(dl, bold_, 12, left, y, kSecondary, name);
        y += S(24);
    };
    /// 一塊圓角的面板，裡面有 rows 列；回傳第一列的位置
    const auto card = [&](float height) {
        dl->AddRectFilled(ImVec2(left, y), ImVec2(left + inner, y + height), kPanel, S(10));
        return y;
    };
    const float field_w = std::min(S(230), inner * 0.45f), field_x = left + inner - S(14) - field_w;
    const auto labels = [&](float row_y, const std::string& name, const std::string& note) {
        text(dl, bold_, 13, left + S(14), row_y + S(9), kText, fitted(bold_, 13, name, field_x - left - S(24)));
        text(dl, regular_, 11.5f, left + S(14), row_y + S(29), kTertiary, fitted(regular_, 11.5f, note, field_x - left - S(24)));
    };
    const auto divider = [&](float row_y) { dl->AddLine(ImVec2(left + S(14), row_y), ImVec2(left + inner, row_y), kSeparator); };
    /// 跳出來的選單（裝置、序列埠、速率）
    const auto menu = [&](const char* id, const Rect& anchor, const std::function<void()>& items) {
        ImGui::SetNextWindowPos(ImVec2(anchor.x, anchor.bottom() + S(4)));
        ImGui::SetNextWindowSizeConstraints(ImVec2(anchor.w, 0), ImVec2(std::max(anchor.w, S(420)), S(360)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(5)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenu);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kMenuSelected);
        ImGui::PushStyleColor(ImGuiCol_Header, kMenuSelected);
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        if (ImGui::BeginPopup(id)) {
            close_popup_on_escape();
            ImGui::PushFont(regular_, 12.5f * scale_);
            items();
            ImGui::PopFont();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
    };

    // ---- 電台
    section(L("電台"));
    {
        const float top = card(row_h);
        labels(top, L("我的呼號"), L("寫進每張畫面的標頭，最多 9 個字元"));
        std::string call = settings.callsign;
        if (input_text("##callsign", Rect{field_x, top + field_y, field_w, S(32)}, call, "", true, true, NBTV_CALLSIGN_MAX)) settings.callsign = normalize_callsign(call);
        y += row_h + S(16);
    }

    // ---- 音訊
    section(L("音訊"));
    {
        const float top = card(row_h * 2);
        const auto device_row = [&](float row_y, const char* id, const std::string& name, const std::string& note, std::string& chosen, const std::vector<AudioDevice>& devices) {
            labels(row_y, name, note);
            const Rect anchor{field_x, row_y + field_y, field_w, S(32)};
            const std::string popup = std::string(id) + "menu";
            if (popup_button(id, anchor, chosen.empty() ? L("系統預設") : chosen)) ImGui::OpenPopup(popup.c_str());
            menu(popup.c_str(), anchor, [&] {
                if (ImGui::Selectable(L("系統預設").c_str(), chosen.empty())) {
                    chosen.clear();
                    app_.devices_changed();
                }
                for (const AudioDevice& device : devices) {
                    if (ImGui::Selectable(device.name.c_str(), device.name == chosen)) {
                        chosen = device.name;
                        app_.devices_changed();
                    }
                }
            });
        };
        device_row(top, "##input", L("輸入裝置"), L("從電台收進來的聲音"), settings.input_device, app_.input_devices);
        divider(top + row_h);
        device_row(top + row_h, "##output", L("輸出裝置"), L("送去電台發射的聲音"), settings.output_device, app_.output_devices);
        y += row_h * 2 + S(16);
    }

    // ---- PTT 控制
    section(L("PTT 控制"));
    {
        const float radio_h = S(31);
        const float top = card(S(12) + radio_h * 4 + S(8) + row_h * 4);
        float ry = top + S(12);
        const std::pair<PTTMode, std::string> choices[] = {{PTTMode::none, L("不控制，使用電台的 VOX")}, {PTTMode::serial, L("序列埠的 RTS 或 DTR")},
                                                           {PTTMode::cat, L("電台的 CAT 指令（經序列埠，由 Hamlib 翻譯）")}, {PTTMode::rigctld, L("Hamlib rigctld（TCP）")}};
        for (const auto& [mode, label] : choices) {
            note_hit("##pttmode/" + std::string(to_string(mode)), Rect{left + S(14), ry, inner - S(28), radio_h});
            ImGui::SetCursorScreenPos(ImVec2(left + S(14), ry));
            ImGui::PushID(static_cast<int>(mode));
            if (ImGui::InvisibleButton("##pttmode", ImVec2(inner - S(28), radio_h))) settings.ptt_mode = mode;
            ImGui::PopID();
            const float cx = left + S(14) + S(8), cy = ry + radio_h / 2;
            if (settings.ptt_mode == mode) {
                dl->AddCircleFilled(ImVec2(cx, cy), S(7.5f), kRx);
                dl->AddCircleFilled(ImVec2(cx, cy), S(3), rgb(0x0C1A1C));
            } else {
                dl->AddCircleFilled(ImVec2(cx, cy), S(7.5f), kWell);
                dl->AddCircle(ImVec2(cx, cy), S(7.5f), kButtonRing, 0, S(1.2f));
            }
            text(dl, regular_, 13, cx + S(18), cy - S(8.5f), kText, label);
            ry += radio_h;
        }
        ry += S(8);

        // 序列埠
        divider(ry);
        labels(ry, L("序列埠"), L("RTS／DTR、CAT 共用"));
        {
            const float line_w = S(112);
            const Rect anchor{field_x - line_w - S(8) + S(50), ry + field_y, field_w - S(50), S(32)};
            const bool have_ports = !app_.serial_ports.empty() || !settings.serial_port.empty();
            const std::string label = settings.serial_port.empty() ? (app_.serial_ports.empty() ? L("（沒有序列埠）") : L("尚未選擇")) : settings.serial_port;
            if (popup_button("##serial", anchor, label, have_ports)) ImGui::OpenPopup("##serialmenu");
            menu("##serialmenu", anchor, [&] {
                for (const std::string& port : app_.serial_ports) {
                    if (ImGui::Selectable(port.c_str(), port == settings.serial_port)) settings.serial_port = port;
                }
            });
            const int line = segmented("##line", Rect{left + inner - S(14) - line_w, ry + field_y, line_w, S(32)}, {"RTS", "DTR"}, settings.serial_line == SerialLine::rts ? 0 : 1);
            settings.serial_line = line == 0 ? SerialLine::rts : SerialLine::dtr;
        }
        ry += row_h;

        // 電台型號（Hamlib 的清單；每隔幾秒才重新找一次有沒有裝）
        divider(ry);
        labels(ry, L("電台型號"), L("用 CAT 指令時要選"));
        {
            if (!hamlib_checked_ || (!hamlib_installed_ && uptime_seconds() - hamlib_checked_at_ > 5)) {
                hamlib_checked_ = true;
                hamlib_checked_at_ = uptime_seconds();
                hamlib_installed_ = hamlib::installed();
                if (hamlib_installed_) rig_models_ = hamlib::models_by_manufacturer();
            }
            const Rect anchor{field_x, ry + field_y, field_w, S(32)};
            const std::string label = !hamlib_installed_ && settings.rig_model <= 0 ? L("（這台電腦沒有裝 Hamlib）") : settings.rig_model > 0 ? settings.rig_model_title() : L("尚未選擇");
            if (popup_button("##rig", anchor, label, hamlib_installed_ && !rig_models_.empty())) ImGui::OpenPopup("##rigmenu");
            if (!hamlib_installed_) tooltip(L("找不到 Hamlib，請先安裝"));
            menu("##rigmenu", anchor, [&] {
                for (const auto& [manufacturer, models] : rig_models_) {
                    if (ImGui::BeginMenu(manufacturer.empty() ? L("其他").c_str() : manufacturer.c_str())) {
                        for (const hamlib::Model& model : models) {
                            if (ImGui::MenuItem(model.name.c_str(), nullptr, model.id == settings.rig_model)) {
                                settings.rig_model = model.id;
                                settings.rig_model_name = model.title();
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
            });
        }
        ry += row_h;

        // 序列速率
        divider(ry);
        labels(ry, L("序列速率"), L("要和電台裡設定的 CAT 速率相同"));
        {
            const Rect anchor{field_x, ry + field_y, field_w, S(32)};
            const std::string label = settings.serial_baud > 0 ? std::to_string(settings.serial_baud) + " bps" : L("依型號的預設");
            if (popup_button("##baud", anchor, label)) ImGui::OpenPopup("##baudmenu");
            menu("##baudmenu", anchor, [&] {
                if (ImGui::Selectable(L("依型號的預設").c_str(), settings.serial_baud == 0)) settings.serial_baud = 0;
                for (const int rate : hamlib::baud_rates()) {
                    if (ImGui::Selectable((std::to_string(rate) + " bps").c_str(), rate == settings.serial_baud)) settings.serial_baud = rate;
                }
            });
        }
        ry += row_h;

        // rigctld 位址
        divider(ry);
        labels(ry, L("rigctld 位址"), L("TCP 連線，本機或遠端都可以"));
        {
            const float port_w = S(74);
            input_text("##righost", Rect{field_x, ry + field_y, field_w - port_w - S(18), S(32)}, settings.rig_host, "127.0.0.1", true, false, 120);
            text(dl, mono_, 13, left + inner - S(14) - port_w - S(12), ry + field_y + S(7), kSecondary, ":");
            input_text("##rigport", Rect{left + inner - S(14) - port_w, ry + field_y, port_w, S(32)}, settings.rig_port, "4532", true, false, 5);
        }
        ry += row_h;
        y = ry + S(16);
    }

    // ---- 顯示
    section(L("顯示"));
    {
        const float top = card(row_h * 2);
        labels(top, L("在收到的畫面上標示訊雜比"), L("畫面與縮圖的左上角，標出收到那一張當下的訊雜比"));
        if (checkbox("##snr", left + inner - S(14) - S(15), top + (row_h - S(18)) / 2, "", settings.show_frame_snr)) settings.show_frame_snr = !settings.show_frame_snr;
        divider(top + row_h);
        labels(top + row_h, L("介面的語言"), L("跟著系統，或自己指定"));
        const Rect anchor{field_x, top + row_h + field_y, field_w, S(32)};
        if (popup_button("##language", anchor, settings.language.empty() ? LF("跟著系統（%@）", {i18n::native_name(i18n::system_language())}) : std::string(i18n::native_name(settings.language)))) {
            ImGui::OpenPopup("##languagemenu");
        }
        menu("##languagemenu", anchor, [&] {
            if (ImGui::Selectable(L("跟著系統").c_str(), settings.language.empty())) settings.language.clear();
            for (const std::string& language : i18n::languages()) {
                if (ImGui::Selectable(i18n::native_name(language), language == settings.language)) settings.language = language;
            }
        });
        y += row_h * 2 + S(24);
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(w - S(12), y - origin.y));
    if (!show_settings) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- 開檔、存檔

void UI::choose_video_file() {
    if (window_ == nullptr) return;
    static const SDL_DialogFileFilter filters[] = {
        {"Video / image", "mp4;mov;m4v;mkv;avi;webm;mpg;mpeg;wmv;flv;ts;jpg;jpeg;png;bmp;gif"},
        {"All files", "*"},
    };
    auto* request = new DialogRequest{[this](const std::string& path) { app_.open_video(path, true); }, {}};
    SDL_ShowOpenFileDialog(dialog_callback, request, window_, filters, 2, nullptr, false);
}

/// 圖片序列：選圖片檔，可以一次選很多張。順序照檔名，和選的先後無關（見 ImageSequence::files）。
void UI::choose_images() {
    if (window_ == nullptr) return;
    // 篩選的清單在對話框開著的這段時間要一直都在：只做一次，之後不再動它
    static const std::string pattern = [] {
        std::string joined;
        for (const std::string& extension : ImageSequence::extensions()) joined += (joined.empty() ? "" : ";") + extension;
        return joined;
    }();
    static const SDL_DialogFileFilter filters[] = {{"Images", pattern.c_str()}, {"All files", "*"}};
    auto* request = new DialogRequest{{}, [this](const std::vector<std::string>& paths) { app_.open_images(paths, true); }};
    SDL_ShowOpenFileDialog(dialog_callback, request, window_, filters, 2, nullptr, true);
}

/// 圖片序列：選一個資料夾，裡面的圖片全部加入（只看那一層）。
void UI::choose_image_folder() {
    if (window_ == nullptr) return;
    auto* request = new DialogRequest{{}, [this](const std::vector<std::string>& paths) { app_.open_images(paths, true); }};
    SDL_ShowOpenFolderDialog(dialog_callback, request, window_, nullptr, false);
}

void UI::save_image() {
    const App::ReceivedFrame* frame = app_.displayed_frame();
    if (window_ == nullptr || frame == nullptr) return;
    static const SDL_DialogFileFilter filters[] = {{"PNG", "png"}};
    static std::string suggested;
    suggested = App::export_file_name(*frame);
    auto* request = new DialogRequest{[this](const std::string& chosen) {
        std::string path = chosen;
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".png") != 0) path += ".png";
        app_.save_displayed_frame(path);
    }, {}};
    SDL_ShowSaveFileDialog(dialog_callback, request, window_, filters, 1, suggested.c_str());
}

void UI::save_video() {
    const std::vector<App::ReceivedFrame> frames = app_.displayed_sequence();
    if (window_ == nullptr || frames.size() < 2) return;
    static const SDL_DialogFileFilter filters[] = {{"MP4", "mp4"}};
    static std::string suggested;
    suggested = App::video_file_name(frames);
    auto* request = new DialogRequest{[this, frames](const std::string& chosen) {
        std::string path = chosen;
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".mp4") != 0) path += ".mp4";
        app_.export_sequence(frames, path);
    }, {}};
    SDL_ShowSaveFileDialog(dialog_callback, request, window_, filters, 1, suggested.c_str());
}

}  // namespace nbtvgui
