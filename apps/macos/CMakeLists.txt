# macOS 外殼：SwiftUI app，只透過 core/include/nbtv/nbtv.h 使用核心。
# 由根目錄的 CMakeLists.txt 在 -DNBTV_BUILD_MACOS_APP=ON 時加入。
enable_language(Swift)

set(NBTV_APP_SOURCES
  Sources/main.swift
  Sources/NBTVApp.swift
  Sources/AppModel.swift
  Sources/RadioEngine.swift
  Sources/CoreBridge.swift
  Sources/AudioDevices.swift
  Sources/AudioIO.swift
  Sources/PTT.swift
  Sources/VideoSources.swift
  Sources/Canvas.swift
  Sources/SequenceVideo.swift
  Sources/Settings.swift
  Sources/Localization.swift
  Sources/Waterfall.swift
  Sources/Theme.swift
  Sources/Controls.swift
  Sources/MainWindow.swift
  Sources/WindowChrome.swift
  Sources/ReceivePanels.swift
  Sources/SidePanel.swift
  Sources/SettingsWindow.swift
  Sources/ModeMenu.swift
  Sources/Schedule.swift
  Sources/ScheduleMenu.swift
  Sources/Demo.swift
  Sources/DebugTap.swift
  Sources/Snapshot.swift
  Sources/SelfTest.swift
)

# 內附的測試圖（96×72 的 PPM）：--snapshot 與 --selftest 用它們當畫面來源。
# 四個模式（11 到 14）的模型權重檔也放進去，每個模式一份；app 啟動後第一次用到時載入。少了哪一份，那個模式就不能用。
# 檔名是模型的名稱（訓練完成的年月日-當天第幾個）：依序是模式 11、12、13、14 用的。
# 最後是接收端「合併最近幾幀」用的權重（每個畫面模型各配一份，小檔案；同樣依序是模式 11、12、13、14 的）；少了的話那個模式照原來的方式解。
set(NBTV_APP_RESOURCES
  ${PROJECT_SOURCE_DIR}/core/tests/data/coffee.ppm
  ${PROJECT_SOURCE_DIR}/core/tests/data/rocket.ppm
  ${PROJECT_SOURCE_DIR}/core/tests/data/cat.ppm
  ${PROJECT_SOURCE_DIR}/models/261123-1.bin
  ${PROJECT_SOURCE_DIR}/models/261122-3.bin
  ${PROJECT_SOURCE_DIR}/models/261122-4.bin
  ${PROJECT_SOURCE_DIR}/models/261123-4.bin
  ${PROJECT_SOURCE_DIR}/models/261124-10.bin
  ${PROJECT_SOURCE_DIR}/models/261124-8.bin
  ${PROJECT_SOURCE_DIR}/models/261124-9.bin
  ${PROJECT_SOURCE_DIR}/models/261124-7.bin
)
# （CMake 的 Swift＋Ninja 不會照 MACOSX_PACKAGE_LOCATION 複製資源，所以在下面建置完成後自己複製。）

# 介面的翻譯：原始碼裡寫的是繁體中文，其他語言在 Localization/<語言>.lproj/Localizable.strings；
# 每個語言另外有一份 InfoPlist.strings（app 的名稱，以及系統詢問相機、麥克風權限時的說明）。
# 啟動時系統照使用者的語言從這幾個裡挑一個，都不合就用英文（Info.plist 的開發語言是 en）。
set(NBTV_APP_LANGUAGES zh-Hant zh-Hans ja es en)
set(NBTV_APP_STRINGS "")
set(NBTV_APP_LPROJ_COMMANDS "")
foreach(language ${NBTV_APP_LANGUAGES})
  list(APPEND NBTV_APP_STRINGS
    "${CMAKE_CURRENT_SOURCE_DIR}/Localization/${language}.lproj/Localizable.strings"
    "${CMAKE_CURRENT_SOURCE_DIR}/Localization/${language}.lproj/InfoPlist.strings")
  list(APPEND NBTV_APP_LPROJ_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Localization/${language}.lproj"
            "$<TARGET_BUNDLE_CONTENT_DIR:nbtv_mac>/Resources/${language}.lproj")
endforeach()
# 找得到 python3 的話，每次建置都檢查翻譯齊不齊（原始碼裡的每一句在每個語言都有、要填的值對得上）；有問題就建不過。
find_program(NBTV_PYTHON3 python3)
set(NBTV_APP_STRINGS_CHECK "")
if(NBTV_PYTHON3)
  set(NBTV_APP_STRINGS_CHECK COMMAND ${NBTV_PYTHON3} "${CMAKE_CURRENT_SOURCE_DIR}/Localization/check_strings.py")
endif()

# Swift 匯入的 C 標頭：列進來，標頭改了才會重新編譯。
set(NBTV_APP_HEADERS
  CNBTV/nbtv_shim.h
  CNBTV/module.modulemap
  ${PROJECT_SOURCE_DIR}/core/include/nbtv/nbtv.h
)
set_source_files_properties(${NBTV_APP_HEADERS} PROPERTIES HEADER_FILE_ONLY ON)

add_executable(nbtv_mac MACOSX_BUNDLE ${NBTV_APP_SOURCES} ${NBTV_APP_HEADERS})
# 翻譯檔改了也要重新連結一次，下面「建置完成後」的那幾步（檢查、複製、簽章）才會再跑
set_property(TARGET nbtv_mac APPEND PROPERTY LINK_DEPENDS ${NBTV_APP_STRINGS} "${CMAKE_CURRENT_SOURCE_DIR}/Localization/check_strings.py")

set_target_properties(nbtv_mac PROPERTIES
  OUTPUT_NAME "NBTV"
  Swift_MODULE_NAME "NBTVApp"
  Swift_LANGUAGE_VERSION 5
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME "NBTV"
  MACOSX_BUNDLE_GUI_IDENTIFIER "tw.hf-nbtv.mac"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)

# 最低支援的系統版本：CMake 的 Swift 支援（Ninja）不會把 CMAKE_OSX_DEPLOYMENT_TARGET 交給 swiftc，
# 所以有指定時在這裡自己轉成 -target（例如 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0）。
# 沒有指定時 swiftc 與 C++ 編譯器都以建置這台機器的系統版本為準。
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
    set(NBTV_APP_ARCH "${CMAKE_OSX_ARCHITECTURES}")
  else()
    set(NBTV_APP_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(NBTV_APP_TRIPLE "${NBTV_APP_ARCH}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
  target_compile_options(nbtv_mac PRIVATE -target ${NBTV_APP_TRIPLE})
  target_link_options(nbtv_mac PRIVATE -target ${NBTV_APP_TRIPLE})
endif()

# Swift 經由 CNBTV/module.modulemap 匯入 nbtv.h。
target_include_directories(nbtv_mac PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/CNBTV")
# 核心是 C++ 靜態函式庫；CMake 看到它的連結語言是 C++，會自動帶上 -lc++。
target_link_libraries(nbtv_mac PRIVATE nbtvcore)

# ad-hoc 簽章：系統的相機／麥克風權限需要 bundle 有簽章才認得。
# swiftc 會把相依檔（NBTV.d）寫在執行檔旁邊，也就是 bundle 的 Contents/MacOS 裡；簽章前要先拿掉。
add_custom_command(TARGET nbtv_mac POST_BUILD
  ${NBTV_APP_STRINGS_CHECK}
  COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE:nbtv_mac>.d"
  # Resources 每次重新建立：換了權重檔的版本時，舊的那一份才不會留在 bundle 裡
  COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_CONTENT_DIR:nbtv_mac>/Resources"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:nbtv_mac>/Resources"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NBTV_APP_RESOURCES} "$<TARGET_BUNDLE_CONTENT_DIR:nbtv_mac>/Resources"
  ${NBTV_APP_LPROJ_COMMANDS}
  COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:nbtv_mac>"
  COMMENT "檢查翻譯、複製資源並做 ad-hoc 簽章：NBTV.app"
  VERBATIM)

# 第二個 app（-DNBTV_MACOS_SECOND_APP=ON）：同一台電腦要同時開兩個、兩部電台互相收發時用。
# 內容是 NBTV.app 的複本，程式完全相同，只改 Info.plist 的名稱與識別碼再重新簽章；
# 識別碼不同，設定（UserDefaults）與系統的麥克風、相機權限就各自獨立。
# NBTVInstance 是自己加的鍵：程式看到它，就在設定視窗的標題標明是哪一個 app。
if(NBTV_MACOS_SECOND_APP)
  set(NBTV_APP2_DIR "${CMAKE_CURRENT_BINARY_DIR}/NBTV 2.app")
  set(NBTV_APP2_PLIST "${NBTV_APP2_DIR}/Contents/Info.plist")
  add_custom_target(nbtv_mac2 ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${NBTV_APP2_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:nbtv_mac>" "${NBTV_APP2_DIR}"
    COMMAND plutil -replace CFBundleIdentifier -string "tw.hf-nbtv.mac2" "${NBTV_APP2_PLIST}"
    COMMAND plutil -replace CFBundleName -string "NBTV 2" "${NBTV_APP2_PLIST}"
    COMMAND plutil -replace CFBundleDisplayName -string "HF NBTV 2" "${NBTV_APP2_PLIST}"
    COMMAND plutil -replace NBTVInstance -string "2" "${NBTV_APP2_PLIST}"
    # 各語言的名稱在 InfoPlist.strings 裡：每一份都在後面加上「 2」。plutil 不能把檔案寫回原來「"鍵" = "值";」的格式，
    # 所以先轉成 XML 再改（.strings 是 XML 的格式系統照樣讀得懂）
    COMMAND /bin/sh -c "for f in \"$0\"/Contents/Resources/*.lproj/InfoPlist.strings; do n=$(plutil -extract CFBundleDisplayName raw -o - \"$f\") && plutil -convert xml1 \"$f\" && plutil -replace CFBundleDisplayName -string \"$n 2\" \"$f\" || exit 1; done" "${NBTV_APP2_DIR}"
    COMMAND codesign --force --sign - "${NBTV_APP2_DIR}"
    COMMENT "複製成第二個 app 並重新簽章：NBTV 2.app"
    VERBATIM)
  add_dependencies(nbtv_mac2 nbtv_mac)
endif()
