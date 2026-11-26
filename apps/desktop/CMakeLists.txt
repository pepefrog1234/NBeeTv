# Windows 與 Linux 的桌面 app（nbtv-gui）：版面照 macOS 版，用 SDL3（視窗、音訊、鏡頭）與 Dear ImGui（介面的元件）寫。
# 和 macOS 的外殼一樣，只透過 core/include/nbtv/nbtv.h 使用核心。
# 由根目錄的 CMakeLists.txt 在 -DNBTV_BUILD_DESKTOP_APP=ON 時加入。macOS 上也編得起來（開發時在那裡試），
# 但發佈給 macOS 的是 apps/macos 那一個。
#
# 用到的別人的原始碼在設定（configure）時下載，版本固定，並核對 SHA-256：
#   SDL 3.4.20（zlib 授權）、Dear ImGui 1.93.0（MIT）、stb_image 與 stb_image_write（公有領域）。
# 都是靜態連結進執行檔，發佈時不必另外帶函式庫。
include(FetchContent)
# CMake 3.24 起下載的壓縮檔預設把解開的檔案時間設成「現在」，並且多了這個參數；較舊的版本不認得它
set(NBTV_FETCH_OPTIONS "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
  set(NBTV_FETCH_OPTIONS DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()

option(NBTV_DESKTOP_SYSTEM_SDL3 "Link nbtv-gui against an installed SDL3 (find_package) instead of building SDL from source" OFF)
option(NBTV_DESKTOP_STATIC_RUNTIME "Linux: link libstdc++ and libgcc statically into nbtv-gui, so it only needs glibc and the system's window/audio libraries" ON)

# ---- SDL3
if(NBTV_DESKTOP_SYSTEM_SDL3)
  find_package(SDL3 3.2 REQUIRED CONFIG)
else()
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  # 用不到的子系統不編：搖桿、震動、感應器、HID
  set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
  set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
  set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
  set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
    ${NBTV_FETCH_OPTIONS})
  FetchContent_MakeAvailable(SDL3)
endif()

# ---- Dear ImGui（它自己沒有 CMakeLists.txt，這裡把需要的幾個檔編成一個函式庫）
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
  URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee
  ${NBTV_FETCH_OPTIONS})
FetchContent_MakeAvailable(imgui)
add_library(nbtv_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(nbtv_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(nbtv_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
target_compile_features(nbtv_imgui PUBLIC cxx_std_20)
target_link_libraries(nbtv_imgui PUBLIC SDL3::SDL3)

# ---- stb：兩個單一標頭檔（讀 JPEG／PNG、寫 PNG）
set(NBTV_STB_DIR "${CMAKE_CURRENT_BINARY_DIR}/stb")
set(NBTV_STB_BASE "https://raw.githubusercontent.com/nothings/stb/6b5799bae8a1fe3792691ef4b72ae26434c3e799")
file(DOWNLOAD "${NBTV_STB_BASE}/stb_image.h" "${NBTV_STB_DIR}/stb_image.h"
  EXPECTED_HASH SHA256=dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020)
file(DOWNLOAD "${NBTV_STB_BASE}/stb_image_write.h" "${NBTV_STB_DIR}/stb_image_write.h"
  EXPECTED_HASH SHA256=5d89edc8bffd848f3e138b2a1dbec67aadbed591894bde16ade7c83dc233af9b)

# ---- 內附的測試圖：core/tests/data 的三張 96×72 PPM 原封不動編進執行檔（--snapshot、--demo、自我測試用，共約 62 KB）
set(NBTV_DEMO_IMAGES_CPP "${CMAKE_CURRENT_BINARY_DIR}/demo_images.cpp")
set(NBTV_DEMO_IMAGES_ARRAYS "")
set(NBTV_DEMO_IMAGES_TABLE "")
foreach(name coffee rocket cat)
  set(image "${PROJECT_SOURCE_DIR}/core/tests/data/${name}.ppm")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${image}")
  file(READ "${image}" hex HEX)
  string(LENGTH "${hex}" hex_length)
  math(EXPR bytes "${hex_length} / 2")
  # 每 32 個位元組換一行（一行太長有的編譯器不收），再把每個位元組寫成 0xNN,
  string(REGEX REPLACE "(................................................................)" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(APPEND NBTV_DEMO_IMAGES_ARRAYS "static const unsigned char k_${name}[] = {\n${hex}\n};\n")
  string(APPEND NBTV_DEMO_IMAGES_TABLE "    {\"${name}\", k_${name}, ${bytes}},\n")
endforeach()
file(CONFIGURE OUTPUT "${NBTV_DEMO_IMAGES_CPP}" @ONLY CONTENT
"// 由 apps/desktop/CMakeLists.txt 從 core/tests/data 的 PPM 產生，不要手改。
namespace nbtvgui::demo {
struct EmbeddedImage {
    const char* name;
    const unsigned char* data;
    unsigned long size;
};
extern const EmbeddedImage kEmbeddedImages[];
extern const int kEmbeddedImageCount;
${NBTV_DEMO_IMAGES_ARRAYS}const EmbeddedImage kEmbeddedImages[] = {
${NBTV_DEMO_IMAGES_TABLE}};
const int kEmbeddedImageCount = 3;
}  // namespace nbtvgui::demo
")

# ---- 翻譯表：建置時把 .strings 變成 C++，並檢查程式裡的每一句在每個語言都有翻譯（少了就建不過）。
# 大部分的句子直接沿用 macOS 版的（apps/macos/Localization）；桌面版自己多出來或說法不同的在 Localization/。
set(NBTV_DESKTOP_SOURCES
  src/main.cpp
  src/i18n.cpp
  src/core_bridge.cpp
  src/audio.cpp
  src/net.cpp
  src/process.cpp
  src/serial.cpp
  src/ptt.cpp
  src/waterfall.cpp
  src/fonts.cpp
  src/sources.cpp
  src/canvas.cpp
  src/settings.cpp
  src/png.cpp
  src/app.cpp
  src/ui.cpp
  src/demo.cpp
  src/offscreen.cpp
  src/snapshot.cpp
  src/selftest.cpp
)
set(NBTV_DESKTOP_HEADERS
  src/i18n.hpp
  src/tasks.hpp
  src/log.hpp
  src/core_bridge.hpp
  src/audio.hpp
  src/net.hpp
  src/process.hpp
  src/serial.hpp
  src/ptt.hpp
  src/waterfall.hpp
  src/fonts.hpp
  src/sources.hpp
  src/canvas.hpp
  src/settings.hpp
  src/png.hpp
  src/app.hpp
  src/ui.hpp
  src/demo.hpp
  src/offscreen.hpp
  src/snapshot.hpp
  src/selftest.hpp
)
set(NBTV_DESKTOP_LANGUAGES zh-Hans ja es en)
set(NBTV_DESKTOP_STRING_FILES "")
foreach(language ${NBTV_DESKTOP_LANGUAGES})
  list(APPEND NBTV_DESKTOP_STRING_FILES
    "${PROJECT_SOURCE_DIR}/apps/macos/Localization/${language}.lproj/Localizable.strings"
    "${CMAKE_CURRENT_SOURCE_DIR}/Localization/${language}.lproj/Localizable.strings")
endforeach()
set(NBTV_DESKTOP_SCANNED "")
foreach(source ${NBTV_DESKTOP_SOURCES} ${NBTV_DESKTOP_HEADERS})
  list(APPEND NBTV_DESKTOP_SCANNED "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
endforeach()

add_executable(nbtv_gen_strings tools/gen_strings.cpp)
target_compile_features(nbtv_gen_strings PRIVATE cxx_std_20)
set(NBTV_DESKTOP_STRINGS_CPP "${CMAKE_CURRENT_BINARY_DIR}/i18n_tables.cpp")
add_custom_command(
  OUTPUT "${NBTV_DESKTOP_STRINGS_CPP}"
  COMMAND nbtv_gen_strings --out "${NBTV_DESKTOP_STRINGS_CPP}"
          --strings "${PROJECT_SOURCE_DIR}/apps/macos/Localization" --strings "${CMAKE_CURRENT_SOURCE_DIR}/Localization"
          --sources ${NBTV_DESKTOP_SCANNED}
  DEPENDS nbtv_gen_strings ${NBTV_DESKTOP_STRING_FILES} ${NBTV_DESKTOP_SCANNED}
  COMMENT "檢查翻譯並產生翻譯表：nbtv-gui"
  VERBATIM)

# ---- app
add_executable(nbtv-gui WIN32 ${NBTV_DESKTOP_SOURCES} ${NBTV_DESKTOP_HEADERS} "${NBTV_DESKTOP_STRINGS_CPP}" "${NBTV_DEMO_IMAGES_CPP}")
target_include_directories(nbtv-gui PRIVATE src "${NBTV_STB_DIR}" "${PROJECT_SOURCE_DIR}/tools")
target_compile_features(nbtv-gui PRIVATE cxx_std_20)
# 權重檔：執行檔旁邊沒有的時候從這裡找（tools/model_file.hpp，和命令列工具同一套規則）
target_compile_definitions(nbtv-gui PRIVATE NBTV_MODEL_DIR="${PROJECT_SOURCE_DIR}/models")
target_link_libraries(nbtv-gui PRIVATE nbtvcore nbtv_imgui SDL3::SDL3)
find_package(Threads REQUIRED)
target_link_libraries(nbtv-gui PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(nbtv-gui PRIVATE ws2_32)
  # <windows.h> 的 min／max 巨集會和 std::min、std::max 打架；少引入一些用不到的東西
  target_compile_definitions(nbtv-gui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  # 命令列參數與檔名一律當成 UTF-8（和命令列工具用同一份 manifest）
  target_sources(nbtv-gui PRIVATE "${PROJECT_SOURCE_DIR}/tools/windows/utf8.manifest")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nbtv-gui PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NBTV_DESKTOP_STATIC_RUNTIME)
  # C++ 的執行階段連進執行檔：下載包不挑系統裝的 libstdc++ 版本，只需要 glibc（多新看建置的那台機器）。
  # 視窗、音訊、鏡頭的函式庫（X11、Wayland、ALSA、PulseAudio、PipeWire…）SDL 在執行時才載入，系統上有哪一個就用哪一個。
  target_link_options(nbtv-gui PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- 連進執行檔的別人的程式：授權聲明隨下載包附上（Dear ImGui 的 MIT 授權要求附上著作權與授權聲明）
set(NBTV_THIRD_PARTY_NOTICES "${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-LICENSES.txt")
set(NBTV_THIRD_PARTY_TEXT "nbtv-gui (the NBeeTv desktop app for Windows and Linux) is statically linked with the following third-party software.\nNBeeTv itself is licensed under the GNU General Public License v3.0 (see LICENSE).\n")
file(READ "${imgui_SOURCE_DIR}/LICENSE.txt" NBTV_IMGUI_LICENSE)
string(APPEND NBTV_THIRD_PARTY_TEXT "\n================================================================\nDear ImGui 1.93.0 - https://github.com/ocornut/imgui\n================================================================\n${NBTV_IMGUI_LICENSE}\n")
if(NOT NBTV_DESKTOP_SYSTEM_SDL3 AND EXISTS "${sdl3_SOURCE_DIR}/LICENSE.txt")
  file(READ "${sdl3_SOURCE_DIR}/LICENSE.txt" NBTV_SDL_LICENSE)
  string(APPEND NBTV_THIRD_PARTY_TEXT "\n================================================================\nSDL 3.4.20 (Simple DirectMedia Layer) - https://libsdl.org\n================================================================\n${NBTV_SDL_LICENSE}\n")
endif()
string(APPEND NBTV_THIRD_PARTY_TEXT "\n================================================================\nstb_image, stb_image_write, stb_truetype, stb_rect_pack, stb_textedit - https://github.com/nothings/stb\n================================================================\nPublic domain (www.unlicense.org) or, at your option, the MIT License:\n\nCopyright (c) 2017 Sean Barrett\n\nPermission is hereby granted, free of charge, to any person obtaining a copy of\nthis software and associated documentation files (the \"Software\"), to deal in\nthe Software without restriction, including without limitation the rights to\nuse, copy, modify, merge, publish, distribute, sublicense, and/or sell copies\nof the Software, and to permit persons to whom the Software is furnished to do\nso, subject to the following conditions:\n\nThe above copyright notice and this permission notice shall be included in all\ncopies or substantial portions of the Software.\n\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\nIMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\nFITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\nAUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\nLIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\nOUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\nSOFTWARE.\n")
file(CONFIGURE OUTPUT "${NBTV_THIRD_PARTY_NOTICES}" @ONLY CONTENT "${NBTV_THIRD_PARTY_TEXT}")

install(TARGETS nbtv-gui RUNTIME DESTINATION "${NBTV_INSTALL_BINDIR}")
install(FILES "${NBTV_THIRD_PARTY_NOTICES}" DESTINATION "${NBTV_INSTALL_DOCDIR}")

# ---- 下載包附上 ffmpeg（另外的程式，不連進執行檔）：指向放著 ffmpeg 執行檔與 FFMPEG-LICENSE.txt 的目錄，安裝時放在 nbtv-gui 旁邊。
# nbtv-gui 先找自己旁邊的 ffmpeg，使用者就不必另外安裝。Windows 的下載包用的是 CI 從原始碼建置的那一個（見 ffmpeg/build-windows.sh）。
set(NBTV_DESKTOP_BUNDLED_FFMPEG "" CACHE PATH "Directory with an ffmpeg executable and FFMPEG-LICENSE.txt to install next to nbtv-gui")
if(NBTV_DESKTOP_BUNDLED_FFMPEG)
  file(TO_CMAKE_PATH "${NBTV_DESKTOP_BUNDLED_FFMPEG}" NBTV_FFMPEG_DIR)
  set(NBTV_FFMPEG_FILES "${NBTV_FFMPEG_DIR}/ffmpeg${CMAKE_EXECUTABLE_SUFFIX}" "${NBTV_FFMPEG_DIR}/FFMPEG-LICENSE.txt")
  foreach(NBTV_FFMPEG_FILE IN LISTS NBTV_FFMPEG_FILES)
    if(NOT EXISTS "${NBTV_FFMPEG_FILE}")
      message(FATAL_ERROR "NBTV_DESKTOP_BUNDLED_FFMPEG: ${NBTV_FFMPEG_FILE} does not exist")
    endif()
  endforeach()
  list(GET NBTV_FFMPEG_FILES 0 NBTV_FFMPEG_EXECUTABLE)
  install(PROGRAMS "${NBTV_FFMPEG_EXECUTABLE}" DESTINATION "${NBTV_INSTALL_BINDIR}")
  install(FILES "${NBTV_FFMPEG_DIR}/FFMPEG-LICENSE.txt" DESTINATION "${NBTV_INSTALL_DOCDIR}")
  message(STATUS "nbtv-gui: the package will include ${NBTV_FFMPEG_EXECUTABLE}")
endif()
