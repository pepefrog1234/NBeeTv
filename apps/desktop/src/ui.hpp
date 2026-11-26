// 介面：用 Dear ImGui 把 macOS 版的版面重畫一次（主視窗、設定、發射模式選單、定時發射的面板）。
// 顏色、尺寸照 macOS 版的 Theme.swift 與版面稿；元件是自己畫的，不是系統原生的。
// 每一圈由主迴圈（或算示範圖的程式）呼叫 draw() 一次，在 ImGui::NewFrame() 與 ImGui::Render() 之間。
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app.hpp"
#include "fonts.hpp"

struct ImFont;
struct ImDrawList;

namespace nbtvgui {

class UI {
public:
    /// window 可以是 nullptr（算示範圖時沒有視窗）：那時候開檔、存檔的對話框不能用。
    UI(App& app, SDL_Renderer* renderer, SDL_Window* window);
    ~UI();
    UI(const UI&) = delete;
    UI& operator=(const UI&) = delete;

    /// 照目前的語言載入字型（要在 ImGui 的 context 建好、還沒開始畫之前呼叫；換語言之後再呼叫一次）。
    void load_fonts();
    /// 介面的放大倍率（高解析度的螢幕）。
    void set_scale(float scale);
    /// 畫整個視窗。width、height 是 ImGui 的顯示大小。
    void draw(float width, float height);

    // 示範圖用：直接指定要不要顯示這幾個面板
    bool show_settings = false;
    /// 主視窗蓋著一層面板（設定）時不畫它底下的東西以外的浮動選單
    void open_mode_menu() { want_mode_menu_ = true; }
    void open_schedule_menu() { want_schedule_menu_ = true; }
    /// 按了「儲存影片」之後跳出來的那一小塊（存檔前的選項）
    void open_save_video_menu() { want_save_video_menu_ = true; }

    /// 找到的字型裡有沒有中文、日文（沒有的話只能顯示英文、西班牙文）
    bool has_cjk_fonts() const { return fonts_found_.has_cjk; }
    /// 一個字型檔都沒找到（用的是 Dear ImGui 內附的那一個，只有英文字母）
    bool has_any_font() const { return !fonts_found_.regular.empty(); }

    /// 測試用：打開之後，每一圈記下可以按的元件畫在哪裡，rect_of 查得到（查的是元件的代號，例如「##transmit」）。
    bool record_hits = false;
    struct HitRect {
        float x = 0, y = 0, w = 0, h = 0;
    };
    std::optional<HitRect> rect_of(const std::string& id) const;

private:
    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
        float right() const { return x + w; }
        float bottom() const { return y + h; }
    };
    enum class Icon {
        settings, download, camera, chevrons, lock, video_file, pause, play, check, clock,
        image_file, previous, next,   // 圖片序列用的：圖片檔、上一張、下一張
    };

    float S(float value) const { return value * scale_; }

    // 元件
    void text(ImDrawList* dl, ImFont* font, float size, float x, float y, std::uint32_t color, const std::string& value) const;
    float text_width(ImFont* font, float size, const std::string& value) const;
    /// 放不下 max_width 時結尾截成「…」
    std::string fitted(ImFont* font, float size, const std::string& value, float max_width) const;
    /// 一段字照寬度折成幾行（中文、日文可以在任何兩個字之間換行）
    std::vector<std::string> wrap(ImFont* font, float size, const std::string& value, float max_width) const;
    float paragraph_height(ImFont* font, float size, const std::string& value, float max_width) const;
    float paragraph(ImDrawList* dl, ImFont* font, float size, float x, float y, float max_width, std::uint32_t color, const std::string& value, bool centered = false) const;
    /// 折行的字，行高是字高的幾倍
    static constexpr float kLineSpacing = 1.42f;
    void icon(ImDrawList* dl, Icon kind, float x, float y, float w, float h, std::uint32_t color, float line_width = 1.5f) const;
    bool button(const char* id, const Rect& rect, const std::string& label, bool enabled = true, Icon* leading = nullptr, const std::string& trailing = {});
    int segmented(const char* id, const Rect& rect, const std::vector<std::string>& labels, int selected, bool enabled = true);
    bool checkbox(const char* id, float x, float y, const std::string& label, bool value);
    bool popup_button(const char* id, const Rect& rect, const std::string& label, bool enabled = true, Icon* leading = nullptr, bool locked = false);
    bool slider(const char* id, const Rect& rect, double& value, double min, double max, std::uint32_t color);
    void level_bar(ImDrawList* dl, const Rect& rect, double fraction, std::uint32_t color) const;
    bool input_text(const char* id, const Rect& rect, std::string& value, const std::string& hint, bool mono = false, bool uppercase = false, std::size_t max_length = 0);
    void tooltip(const std::string& value);
    void picture(ImDrawList* dl, const PicturePtr& picture, const Rect& rect, float rounding = 0, int round_flags = 0);

    // 版面
    void draw_top_bar(const Rect& rect);
    void draw_bottom_bar(const Rect& rect);
    void draw_picture_panel(const Rect& rect);
    void draw_signal_panel(const Rect& rect);
    void draw_recent_panel(const Rect& rect);
    void draw_spectrum(const Rect& rect);
    void draw_side_panel(const Rect& rect);
    void draw_mode_menu();
    void draw_schedule_menu();
    void draw_save_video_menu();
    void draw_settings();
    void draw_ptt_warning(const Rect& rect);
    void draw_snr_badge(ImDrawList* dl, float x, float y, double snr_db, float size) const;

    // 開檔、存檔
    void choose_video_file();
    void choose_images();
    void choose_image_folder();
    void save_image();
    void save_video();

    SDL_Texture* texture_for(const PicturePtr& picture);
    void note_hit(const std::string& id, const Rect& rect);
    void apply_style();
    void close_popup_on_escape();
    void update_waterfall_texture();

    App& app_;
    SDL_Renderer* renderer_;
    SDL_Window* window_;
    float scale_ = 1.0f;
    FontSet fonts_found_;
    ImFont* regular_ = nullptr;
    ImFont* bold_ = nullptr;
    ImFont* mono_ = nullptr;
    ImFont* mono_bold_ = nullptr;

    struct CachedTexture {
        SDL_Texture* texture = nullptr;
        std::uint64_t used = 0;
        PicturePtr keep;   // 握著那張畫面：指標才不會被別的畫面重複使用
    };
    std::map<const Picture*, CachedTexture> textures_;
    std::uint64_t frame_counter_ = 0;
    SDL_Texture* waterfall_texture_ = nullptr;
    std::uint64_t waterfall_version_ = 0;

    std::map<std::string, Rect> hits_;
    bool want_mode_menu_ = false;
    bool want_schedule_menu_ = false;
    bool want_save_video_menu_ = false;
    Rect mode_anchor_;
    Rect schedule_anchor_;
    Rect save_video_anchor_;
    std::string canvas_buffer_;
    bool hamlib_checked_ = false;
    bool hamlib_installed_ = false;
    std::vector<std::pair<std::string, std::vector<hamlib::Model>>> rig_models_;
    double hamlib_checked_at_ = -1e9;
};

}  // namespace nbtvgui
