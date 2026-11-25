#include "offscreen.hpp"

#include <cmath>
#include <set>

#include "imgui.h"
#include "imgui_impl_sdlrenderer3.h"
#include "png.hpp"
#include "tasks.hpp"

namespace nbtvgui {

OffscreenUI::OffscreenUI(App& app, int width, int height, float density)
    : app_(app), width_(width), height_(height), density_(density), pixel_width_(static_cast<int>(std::lround(width * density))),
      pixel_height_(static_cast<int>(std::lround(height * density))) {
    surface_ = SDL_CreateSurface(pixel_width_, pixel_height_, SDL_PIXELFORMAT_RGBA32);
    if (surface_ == nullptr) {
        error_ = SDL_GetError();
        return;
    }
    renderer_ = SDL_CreateSoftwareRenderer(surface_);
    if (renderer_ == nullptr) {
        error_ = SDL_GetError();
        return;
    }
    previous_ = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
    ui_ = std::make_unique<UI>(app_, renderer_, nullptr);
    ui_->record_hits = true;
    ui_->load_fonts();
}

OffscreenUI::~OffscreenUI() {
    if (context_ != nullptr) {
        ImGui::SetCurrentContext(context_);
        ui_.reset();   // 貼圖要在 renderer 還在的時候還回去
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previous_);
    }
    ui_.reset();
    if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    if (surface_ != nullptr) SDL_DestroySurface(surface_);
}

void OffscreenUI::frames(int count) {
    if (!ok()) return;
    ImGui::SetCurrentContext(context_);
    for (int i = 0; i < count; ++i) {
        MainQueue::shared().drain();
        app_.tick();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(width_), static_cast<float>(height_));
        io.DisplayFramebufferScale = ImVec2(density_, density_);
        io.DeltaTime = 1.0f / 30.0f;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();
        ui_->draw(static_cast<float>(width_), static_cast<float>(height_));
        ImGui::Render();
        SDL_SetRenderScale(renderer_, density_, density_);   // 後端不會自己放大：照「一點幾個像素」設定
        SDL_SetRenderDrawColor(renderer_, 0x12, 0x15, 0x1A, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }
}

void OffscreenUI::reload_fonts() {
    if (!ok()) return;
    ImGui::SetCurrentContext(context_);
    ui_->load_fonts();
}

void OffscreenUI::move_mouse(float x, float y) {
    if (!ok()) return;
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().AddMousePosEvent(x, y);
    frames(1);
}

void OffscreenUI::click(float x, float y) {
    if (!ok()) return;
    move_mouse(x, y);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frames(1);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frames(2);
}

bool OffscreenUI::click(const std::string& id) {
    if (!ok()) return false;
    const std::optional<UI::HitRect> rect = ui_->rect_of(id);
    if (!rect) return false;
    click(rect->x + rect->w / 2, rect->y + rect->h / 2);
    return true;
}

void OffscreenUI::type_text(const std::string& text) {
    if (!ok()) return;
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().AddInputCharactersUTF8(text.c_str());
    frames(2);
}

void OffscreenUI::press_escape() {
    if (!ok()) return;
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frames(1);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frames(2);
}

std::vector<std::uint8_t> OffscreenUI::pixels() const {
    std::vector<std::uint8_t> rgb;
    if (surface_ == nullptr) return rgb;
    rgb.resize(static_cast<std::size_t>(pixel_width_) * pixel_height_ * 3);
    if (!SDL_LockSurface(surface_)) return {};
    for (int y = 0; y < pixel_height_; ++y) {
        const std::uint8_t* row = static_cast<const std::uint8_t*>(surface_->pixels) + static_cast<std::size_t>(y) * static_cast<std::size_t>(surface_->pitch);
        std::uint8_t* out = rgb.data() + static_cast<std::size_t>(y) * pixel_width_ * 3;
        for (int x = 0; x < pixel_width_; ++x) {
            out[x * 3] = row[x * 4];
            out[x * 3 + 1] = row[x * 4 + 1];
            out[x * 3 + 2] = row[x * 4 + 2];
        }
    }
    SDL_UnlockSurface(surface_);
    return rgb;
}

bool OffscreenUI::save_png(const std::string& path) const {
    const std::vector<std::uint8_t> rgb = pixels();
    return !rgb.empty() && write_png(path, rgb.data(), pixel_width_, pixel_height_, 3);
}

int OffscreenUI::distinct_colors(int limit) const {
    const std::vector<std::uint8_t> rgb = pixels();
    std::set<std::uint32_t> seen;
    // 每隔幾個像素看一個就夠了
    for (std::size_t i = 0; i + 2 < rgb.size() && static_cast<int>(seen.size()) < limit; i += 3 * 7) {
        seen.insert(static_cast<std::uint32_t>(rgb[i]) << 16 | static_cast<std::uint32_t>(rgb[i + 1]) << 8 | rgb[i + 2]);
    }
    return static_cast<int>(seen.size());
}

}  // namespace nbtvgui
