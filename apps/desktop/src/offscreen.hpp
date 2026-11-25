// 不開視窗把介面算成圖：SDL 的軟體繪圖畫到一塊記憶體裡。示範圖（--snapshot）與自我測試（--selftest）共用——
// 測試時還可以假裝滑鼠在某個位置按一下，檢查按鈕真的有作用。不需要螢幕、顯示卡，CI 的機器上也能跑。
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "ui.hpp"

struct ImGuiContext;

namespace nbtvgui {

class OffscreenUI {
public:
    /// width、height：介面的大小（點）。density：一點畫成幾個像素（2 就是高解析度螢幕的樣子）。
    OffscreenUI(App& app, int width, int height, float density = 2.0f);
    ~OffscreenUI();
    OffscreenUI(const OffscreenUI&) = delete;
    OffscreenUI& operator=(const OffscreenUI&) = delete;

    bool ok() const { return renderer_ != nullptr; }
    const std::string& error() const { return error_; }
    UI& ui() { return *ui_; }

    /// 畫 count 圈（跳出來的選單要到第二圈才看得到）。
    void frames(int count = 3);
    /// 照目前的語言重新載入字型（換語言之後呼叫）。
    void reload_fonts();

    // 假裝的滑鼠與鍵盤（每個動作之間各畫一圈）
    void move_mouse(float x, float y);
    void click(float x, float y);
    /// 按一下代號是 id 的元件的正中間；這一圈沒有畫出那個元件時回傳 false。
    bool click(const std::string& id);
    void type_text(const std::string& text);
    void press_escape();

    int pixel_width() const { return pixel_width_; }
    int pixel_height() const { return pixel_height_; }
    /// 最後一圈畫出來的樣子：RGB，由左上角開始逐列。
    std::vector<std::uint8_t> pixels() const;
    bool save_png(const std::string& path) const;
    /// 畫面上有幾種不一樣的顏色（檢查「不是一片空白」用），最多數到 limit。
    int distinct_colors(int limit = 64) const;

private:
    App& app_;
    int width_, height_;
    float density_;
    int pixel_width_, pixel_height_;
    std::string error_;
    SDL_Surface* surface_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ImGuiContext* context_ = nullptr;
    ImGuiContext* previous_ = nullptr;
    std::unique_ptr<UI> ui_;
};

}  // namespace nbtvgui
