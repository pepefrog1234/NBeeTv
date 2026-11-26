cmake_minimum_required(VERSION 3.20)
project(hf_nbtv VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NBTV_BUILD_TESTS "Build the unit tests" ON)
option(NBTV_BUILD_TOOLS "Build the command line tools" ON)
option(NBTV_BUILD_MACOS_APP "Build the macOS app (SwiftUI shell)" OFF)
option(NBTV_MACOS_SECOND_APP "Also build NBTV 2.app, a second copy with its own settings, to run two at once" OFF)
option(NBTV_BUILD_DESKTOP_APP "Build nbtv-gui, the desktop app for Windows and Linux (SDL3 + Dear ImGui, fetched at configure time)" OFF)
option(NBTV_INSTALL_FLAT "Install the tools, models/ and documents side by side in one directory (portable package) instead of bin/ and share/" OFF)
option(NBTV_STATIC_TOOLS "Link the command line tools fully statically (Linux: no dependency on the distribution or its glibc)" OFF)

if(MSVC)
  # 原始碼是 UTF-8，裡面的字串（說明與訊息是中文）也照 UTF-8 編進執行檔；
  # fopen、getenv 這些標準函式不必提醒改用微軟自己的版本。
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# 安裝的位置（cmake --install <建置目錄> --prefix <目錄>）。預設是一般的配置：bin/ 與 share/nbtv/models/。
# -DNBTV_INSTALL_FLAT=ON 改成全部放在同一層（執行檔旁邊就是 models/），整個目錄可以直接打包給別人，
# GitHub 上的下載包就是這樣做的。兩種配置工具都找得到權重檔（tools/model_file.hpp）。
include(GNUInstallDirs)
if(NBTV_INSTALL_FLAT)
  set(NBTV_INSTALL_BINDIR ".")
  set(NBTV_INSTALL_MODELDIR "models")
  set(NBTV_INSTALL_DOCDIR ".")
else()
  set(NBTV_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
  set(NBTV_INSTALL_MODELDIR "${CMAKE_INSTALL_DATADIR}/nbtv/models")
  set(NBTV_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/nbtv")
endif()

add_subdirectory(core)
add_subdirectory(sim)

if(NBTV_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
# 權重檔與說明：命令列工具與桌面 app 都要用，有建其中一樣就裝
if(NBTV_BUILD_TOOLS OR NBTV_BUILD_DESKTOP_APP)
  install(DIRECTORY models/ DESTINATION "${NBTV_INSTALL_MODELDIR}" FILES_MATCHING PATTERN "*.bin")
  install(FILES LICENSE README.md README.zh-CN.md README.ja.md README.es.md README.en.md DESTINATION "${NBTV_INSTALL_DOCDIR}")
endif()

if(NBTV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
endif()

if(APPLE AND NBTV_BUILD_MACOS_APP)
  add_subdirectory(apps/macos)
endif()

if(NBTV_BUILD_DESKTOP_APP)
  add_subdirectory(apps/desktop)
endif()
