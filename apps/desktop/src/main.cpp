// nbtv-gui：Windows 與 Linux 的桌面 app。
//   nbtv-gui                            開視窗
//   nbtv-gui --demo                     開視窗，裡面是示範資料，不碰任何裝置（看版面用）
//   nbtv-gui --selftest [--only 名稱]     不開視窗、不碰任何裝置，逐項檢查（有失敗的項目時結束碼是 1）
//   nbtv-gui --snapshot 目錄 [--lang 語言]  不開視窗、不碰任何裝置，把幾張版面各算成一張 PNG（--lang all：五種語言都算）
//   nbtv-gui --version
// 可以一起用的：
//   --settings 檔案      設定存在這個檔案（預設在系統給這個 app 的設定目錄裡）
//   --report 檔案        --selftest、--snapshot 印的內容也寫一份進這個檔案
//   --quit-after 秒數    開視窗之後過這麼久自己結束（檢查「開得起來」用）
// 給自己用的：
//   nbtv-gui --ptt-watchdog 連接埠 行程   見 ptt.cpp 的 HamlibPTT
//   nbtv-gui --sleep 秒數                什麼都不做，過幾秒結束（自我測試拿它當「別的行程」）
//   nbtv-gui --read-image 圖片檔 [輸出.png]  照「圖片序列」讀圖的方式把一張圖讀進來（照記載的方向轉正、縮小），印出大小；
//                                        有給輸出檔就把讀進來的樣子存成 PNG。用來看某一張真的相片這個程式讀成什麼樣子
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "core_bridge.hpp"
#include "i18n.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "log.hpp"
#include "net.hpp"
#include "png.hpp"
#include "ptt.hpp"
#include "selftest.hpp"
#include "snapshot.hpp"
#include "startup.hpp"
#include "tasks.hpp"
#include "ui.hpp"

namespace {

using namespace nbtvgui;

struct Options {
    enum class Mode { window, selftest, snapshot, version, help };
    Mode mode = Mode::window;
    bool demo = false;
    std::string only;
    std::string snapshot_directory;
    std::string language;
    std::string settings_path;
    std::string work_directory;
    std::optional<double> quit_after;
};

#if defined(_WIN32)
/// 視窗程式預設沒有主控台。從命令列啟動、輸出又沒有導到別的地方時，接到那個命令列視窗上，--help、--selftest 印的東西才看得到。
void attach_parent_console() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;   // 已經導到檔案或管線
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    std::FILE* ignored = nullptr;
    freopen_s(&ignored, "CONOUT$", "w", stdout);
    freopen_s(&ignored, "CONOUT$", "w", stderr);
}
#endif

void print_help() {
    log::line("nbtv-gui：短波 NBTV 的桌面 app（Windows、Linux）");
    log::line("  nbtv-gui                             開視窗");
    log::line("  nbtv-gui --demo                      開視窗，裡面是示範資料，不碰任何裝置");
    log::line("  nbtv-gui --selftest [--only 名稱]      不開視窗、不碰任何裝置，逐項檢查");
    log::line("  nbtv-gui --snapshot 目錄 [--lang 語言]   把幾張版面各算成一張 PNG（語言：zh-Hant、zh-Hans、ja、es、en、all）");
    log::line("  nbtv-gui --version");
    log::line("  共用的選項：--settings 檔案、--report 檔案、--quit-after 秒數");
}

/// 介面要放大幾倍（以視窗的座標為單位）：Windows、X11 上視窗的座標就是像素，高解析度的螢幕要自己放大；
/// macOS、Wayland 上視窗的座標是「點」，系統已經放大過了，這裡是 1。
float window_scale(SDL_Window* window) {
    const float display = SDL_GetWindowDisplayScale(window), density = SDL_GetWindowPixelDensity(window);
    if (display <= 0 || density <= 0) return 1.0f;
    return std::clamp(display / density, 0.75f, 4.0f);
}

/// 這個語言要中文、日文的字型才顯示得出來
bool needs_cjk(const std::string& language) { return language == "zh-Hant" || language == "zh-Hans" || language == "ja"; }

struct Loop {
    Options options;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<App> app;
    std::unique_ptr<UI> ui;
    float scale = 1.0f;
    std::string applied_language;   // 設定裡的語言上次看到的值（空字串：跟著系統）
    bool language_fallback = false; // 找不到中文、日文的字型，介面改用英文
    bool running = true;
    bool pumping = false;           // 正在等事件（視窗被拖著改大小時，系統會卡在這裡面）
    bool stepping = false;
    double started = 0;
    double last_input = 0;
    double last_save = 0;
    long frames = 0;
    std::vector<std::string> dropped;   // 拖進視窗、還沒處理的檔案（一次拖好幾個時要先收齊）
    double dropped_at = 0;              // 最近一個是什麼時候收到的

    /// 照設定（或系統）的語言設定介面的語言、載入字型、改視窗的標題。
    void apply_language() {
        applied_language = app->settings.language;
        std::string language = applied_language.empty() ? i18n::system_language() : applied_language;
        i18n::use(language);
        ui->load_fonts();
        language_fallback = false;
        if (needs_cjk(language) && !ui->has_cjk_fonts()) {
            // 這台電腦上找不到中文、日文的字型：照原來的語言畫會整片都是方框，改用英文
            i18n::use("en");
            ui->load_fonts();
            language_fallback = true;
            app->notice = L("找不到中文、日文的字型，介面改用英文顯示（可以安裝 Noto Sans CJK 這類字型）");
        }
        if (window != nullptr) SDL_SetWindowTitle(window, L("短波 NBTV").c_str());
    }

    /// 做一圈：把別的執行緒交回來的事做完、推進收發的流程、畫一格。
    void step() {
        if (stepping) return;
        stepping = true;
        const double now = uptime_seconds();
        MainQueue::shared().drain();
        app->tick();
        if (app->settings.language != applied_language) apply_language();
        if (now - last_save > 1.0) {
            last_save = now;
            app->settings.save();
        }
        const float current_scale = window_scale(window);
        if (std::fabs(current_scale - scale) > 0.01f) {
            scale = current_scale;
            ui->set_scale(scale);
        }
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) == 0) {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            const ImGuiIO& io = ImGui::GetIO();
            ui->draw(io.DisplaySize.x, io.DisplaySize.y);
            ImGui::Render();
            // 高解析度的螢幕（macOS、Wayland）：一點是好幾個像素，繪圖的後端不會自己放大
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 0x12, 0x15, 0x1A, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            ++frames;
        }
        stepping = false;
    }
};

/// 視窗被拖著移動、改大小的時候，有些系統（Windows、macOS）會把主執行緒卡在它自己的迴圈裡，直到放開滑鼠才回來；
/// 這段時間 SDL 會用「視窗要重畫」的事件通知這裡。照樣做一圈：畫面跟著更新，更重要的是發射中的排程不能停。
bool SDLCALL expose_watch(void* userdata, SDL_Event* event) {
    Loop* loop = static_cast<Loop*>(userdata);
    if (event->type == SDL_EVENT_WINDOW_EXPOSED && loop->pumping && SDL_IsMainThread()) loop->step();
    return true;
}

int fatal(const std::string& text) {
    log::line("nbtv-gui：" + text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NBeeTv", text.c_str(), nullptr);
    return 1;
}

int run_window(const Options& options) {
    SDL_SetAppMetadata("NBeeTv", nbtv_version_string(), "tw.yakumo.nbeetv");
    if (!SDL_Init(SDL_INIT_VIDEO)) return fatal(std::string("SDL_Init: ") + SDL_GetError());

    Loop loop;
    loop.options = options;
    std::string audio_problem;
    if (options.demo) {
        i18n::use(options.language.empty() ? i18n::system_language() : options.language);
        // 開發用：設了 NBTV_SNAPSHOT_FM 時，示範資料換成給 FM 用的大畫面（192×144）那一份，在真的視窗裡看得到大畫面的樣子
        // （值是某個大畫面模式的編號時，示範裡選的發射模式用它）。app 裡沒有那幾個模式的模型時照平常的示範資料
        if (const char* fm = std::getenv("NBTV_SNAPSHOT_FM")) loop.app = snapshot::make_fm_app(std::atoi(fm), true);
        if (!loop.app) loop.app = snapshot::make_receiving_app(true);
        loop.app->settings.language = options.language;
    } else {
        // 音訊或鏡頭的子系統起不來，視窗還是要開：問題寫在底列
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) audio_problem = SDL_GetError();
        if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) log::line(std::string("nbtv-gui：鏡頭的子系統起不來：") + SDL_GetError());
        std::string path = options.settings_path;
        if (path.empty()) {
            if (char* folder = SDL_GetPrefPath("NBeeTv", "nbtv-gui")) {
                path = std::string(folder) + "settings.txt";
                SDL_free(folder);
            }
        }
        Settings settings = Settings::load(path);
        if (!options.language.empty()) settings.language = options.language;
        loop.app = std::make_unique<App>(std::move(settings), true);
    }
    App& app = *loop.app;

    // 視窗：預設 1280×840（放大倍率照主要的螢幕），不超過螢幕可以用的範圍
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    float initial_scale = SDL_GetDisplayContentScale(display);
    if (initial_scale <= 0) initial_scale = 1.0f;
    int width = static_cast<int>(std::lround((app.settings.window_width > 0 ? app.settings.window_width : 1280) * initial_scale));
    int height = static_cast<int>(std::lround((app.settings.window_height > 0 ? app.settings.window_height : 840) * initial_scale));
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 200 && usable.h > 200) {
        width = std::min(width, usable.w - 16);
        height = std::min(height, usable.h - 48);
    }
    loop.window = SDL_CreateWindow("NBeeTv", width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (loop.window == nullptr) return fatal(std::string("SDL_CreateWindow: ") + SDL_GetError());
    loop.renderer = SDL_CreateRenderer(loop.window, nullptr);
    if (loop.renderer == nullptr) loop.renderer = SDL_CreateRenderer(loop.window, SDL_SOFTWARE_RENDERER);
    if (loop.renderer == nullptr) return fatal(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    SDL_SetRenderVSync(loop.renderer, 1);
    loop.scale = window_scale(loop.window);
    SDL_SetWindowMinimumSize(loop.window, std::min(width, static_cast<int>(1040 * loop.scale)), std::min(height, static_cast<int>(700 * loop.scale)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLRenderer(loop.window, loop.renderer);
    ImGui_ImplSDLRenderer3_Init(loop.renderer);
    loop.ui = std::make_unique<UI>(app, loop.renderer, loop.window);
    loop.ui->set_scale(loop.scale);
    loop.apply_language();

    const Uint32 wake_event = SDL_RegisterEvents(1);
    MainQueue::shared().set_waker([wake_event] {
        SDL_Event event;
        SDL_zero(event);
        event.type = wake_event;
        SDL_PushEvent(&event);
    });
    SDL_AddEventWatch(expose_watch, &loop);

    app.start();
    if (!audio_problem.empty()) app.notice = LF("音訊的子系統起不來：%@", {audio_problem});
    SDL_ShowWindow(loop.window);
    loop.started = uptime_seconds();
    loop.last_input = loop.started;
    log::line(std::string("nbtv-gui：視窗 ") + std::to_string(width) + "×" + std::to_string(height) + "，放大 " + std::to_string(loop.scale) + "，繪圖 " +
              (SDL_GetRendererName(loop.renderer) != nullptr ? SDL_GetRendererName(loop.renderer) : "?") + "，音訊 " +
              (SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "（沒有）") + "，語言 " + i18n::language());

    while (loop.running) {
        const double now = uptime_seconds();
        const bool minimized = (SDL_GetWindowFlags(loop.window) & SDL_WINDOW_MINIMIZED) != 0;
        // 發射中每 20 毫秒看一次進度；平常有人在操作時畫得勤一點，沒有的話一秒十次就夠（收到新的資料會被叫醒）
        const bool busy = app.tx_phase != App::TxPhase::idle;
        const int timeout = busy ? 20 : minimized ? 200 : now - loop.last_input < 2.0 ? 16 : 100;
        SDL_Event event;
        loop.pumping = true;
        bool got = SDL_WaitEventTimeout(&event, timeout);
        bool drop_complete = false;
        while (got) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) loop.running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(loop.window)) loop.running = false;
            // 把檔案拖進視窗：一次拖好幾個的話，系統是一個檔案一個事件接連送來的，最後再說「這一批到此為止」。先收齊
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
                loop.dropped.emplace_back(event.drop.data);
                loop.dropped_at = now;
            }
            if (event.type == SDL_EVENT_DROP_COMPLETE) drop_complete = true;
            if (event.type != wake_event) loop.last_input = now;
            got = SDL_PollEvent(&event);
        }
        loop.pumping = false;
        // 拖進來的檔案：影片檔（或一張圖片）當成在「影片檔」那裡選了它，好幾張圖片或資料夾當成圖片序列（見 App::open_dropped）。
        // 等到「這一批到此為止」才處理；萬一等不到那個事件，過了三分之一秒也照樣處理
        if (!loop.dropped.empty() && (drop_complete || now - loop.dropped_at > 0.33)) {
            if (app.live) app.open_dropped(loop.dropped);
            loop.dropped.clear();
        }
        if (options.quit_after && now - loop.started > *options.quit_after) loop.running = false;
        loop.step();
    }

    // 結束：停止輸出、放開 PTT（同步做完）、存設定
    const double seconds = uptime_seconds() - loop.started;
    if ((SDL_GetWindowFlags(loop.window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) == 0) {
        int w = 0, h = 0;
        SDL_GetWindowSize(loop.window, &w, &h);
        if (w > 0 && h > 0 && loop.scale > 0) {
            app.settings.window_width = static_cast<int>(std::lround(w / loop.scale));
            app.settings.window_height = static_cast<int>(std::lround(h / loop.scale));
        }
    }
    SDL_RemoveEventWatch(expose_watch, &loop);
    app.shutdown();
    MainQueue::shared().set_waker(nullptr);
    log::line("nbtv-gui：結束，開了 " + std::to_string(seconds) + " 秒，畫了 " + std::to_string(loop.frames) + " 格");
    loop.ui.reset();
    loop.app.reset();
    MainQueue::shared().drain();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(loop.renderer);
    SDL_DestroyWindow(loop.window);
    SDL_Quit();
    return 0;
}

/// --read-image：照圖片序列讀圖的方式讀一張圖，印出結果；out 不是空的話另外存成 PNG。
int read_image(const std::string& path, const std::string& out) {
    const SourceFramePtr frame = ImageSequence::load(path);
    if (!frame) {
        log::line("nbtv-gui：讀不進來：" + path + (ImageSequence::is_image(path) ? "" : "（副檔名不是認得的圖片類型；放進資料夾一起選的話會被略過）"));
        return 1;
    }
    log::line("nbtv-gui：" + path + "：原圖轉正之後 " + std::to_string(frame->shown_width()) + "×" + std::to_string(frame->shown_height()) + "，讀進來留著 " +
              std::to_string(frame->width) + "×" + std::to_string(frame->height) + (ImageSequence::is_image(path) ? "" : "（副檔名不是認得的圖片類型；放進資料夾一起選的話會被略過）"));
    if (!out.empty() && !write_png(out, frame->rgb.data(), frame->width, frame->height, 3)) {
        log::line("nbtv-gui：寫不出 " + out);
        return 1;
    }
    return 0;
}

/// --selftest、--snapshot：不開視窗。SDL 的視訊用「假的」那一個（不需要螢幕），音訊只有在指定用假的音訊驅動時才啟動。
void init_headless() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) log::line(std::string("nbtv-gui：注意：SDL 的假視訊驅動起不來（") + SDL_GetError() + "），離屏繪圖照樣試");
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    attach_parent_console();
#endif
    nbtv::tools::tool_startup();
    nbtvgui::net::startup();
    const std::vector<std::string> args(argv + 1, argv + argc);

    // 給自己用的兩個
    if (args.size() >= 3 && args[0] == "--ptt-watchdog") return nbtvgui::ptt_watchdog_main(args[1], args[2]);
    if (args.size() >= 2 && args[0] == "--sleep") {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(std::atof(args[1].c_str()) * 1000)));
        return 0;
    }
    if (args.size() >= 2 && args[0] == "--read-image") return read_image(args[1], args.size() >= 3 ? args[2] : std::string());

    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (arg == "--selftest") options.mode = Options::Mode::selftest;
        else if (arg == "--snapshot") {
            options.mode = Options::Mode::snapshot;
            options.snapshot_directory = value();
        } else if (arg == "--version") options.mode = Options::Mode::version;
        else if (arg == "--help" || arg == "-h") options.mode = Options::Mode::help;
        else if (arg == "--demo") options.demo = true;
        else if (arg == "--only") options.only = value();
        else if (arg == "--lang") options.language = value();
        else if (arg == "--settings") options.settings_path = value();
        else if (arg == "--work") options.work_directory = value();
        else if (arg == "--report") {
            log::report_path() = value();
            if (!log::report_path().empty()) SDL_RemovePath(log::report_path().c_str());   // 每次從頭寫
        } else if (arg == "--quit-after") options.quit_after = std::atof(value().c_str());
        else {
            log::line("nbtv-gui：不認得的參數 " + arg);
            print_help();
            return 2;
        }
    }

    switch (options.mode) {
    case Options::Mode::help:
        print_help();
        return 0;
    case Options::Mode::version:
        log::line(std::string("nbtv-gui ") + nbtv_version_string());
        // 這台電腦上畫面的編解碼用的是哪一種寫法、幾條執行緒（回報「會卡」的時候附上這一行，比較好判斷）
        log::line(std::string("畫面的編解碼：") + nbtv_inference_kernel() + "，最多 " + std::to_string(nbtv_inference_threads()) + " 條執行緒");
        // 影片檔來源與存影片用的是哪一個 ffmpeg（Windows 的下載包附了一個，放在 nbtv-gui.exe 旁邊；CI 用這一行確認用的是它）
        if (const std::string ffmpeg = FileSource::ffmpeg_path(); !ffmpeg.empty()) log::line("ffmpeg：" + ffmpeg);
        else log::line("ffmpeg：找不到（影片檔來源與存影片不能用）");
        return 0;
    case Options::Mode::selftest: {
        init_headless();
        nbtvgui::selftest::Options test;
        test.only = options.only;
        test.work_directory = options.work_directory;
        test.executable = nbtv::tools::executable_path();
        const int code = nbtvgui::selftest::run(test);
        SDL_Quit();
        return code;
    }
    case Options::Mode::snapshot: {
        if (options.snapshot_directory.empty()) {
            print_help();
            return 2;
        }
        init_headless();
        std::vector<std::string> languages = {options.language.empty() ? std::string("zh-Hant") : options.language};
        if (options.language == "all") languages = nbtvgui::i18n::languages();
        const int code = nbtvgui::snapshot::run(options.snapshot_directory, languages);
        SDL_Quit();
        return code;
    }
    case Options::Mode::window:
        break;
    }
    return run_window(options);
}
