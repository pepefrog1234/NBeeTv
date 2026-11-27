# 即時語音的編解碼器（include/nbtv/voice.h）：給「語音與畫面共用 FM 語音通道」用。
# 預設只有空殼：nbtv_voice_available() 回傳 0，不需要任何外部的東西，外殼照樣可以連結它。
#
# -DNBTV_VOICE=ON 才把 FreeDV 的 RADE V2 編進來。設定（configure）時要另外取得三份檔案，版本都固定，並核對 SHA-256
# （哪一個版本、從哪裡抓寫在 cmake/sources.cmake）：
#   rade_c          RADE 的 C 實作，權重編在程式裡（BSD 兩條款）
#   Opus            語音的特徵（LPCNet）與合成（FARGAN）在它裡面（BSD 三條款）
#   Opus 的模型資料   約 184 MB，只取用得到的三組權重
# rade_c 只取原始碼；Opus 用它自己的 CMakeLists 建成靜態函式庫（只開語音用得到的那一部分），所以不需要 autotools，
# Linux、macOS、Windows 的建法都一樣。有兩處直接改取下來的原始碼（每次設定時檢查，已經改過就不動）：
#   * Opus 的 dnn/nnet.c：類神經網路層的輸入上限加大（rade_c 要求的）
#   * Windows 上 rade_c 的 rade_api.h：拿掉給動態函式庫用的匯出標記（這裡是靜態連結）
#
# 三份檔案可以先抓好放在一個資料夾，設定時就不必連網路（CI 就是這樣做的）：
#   cmake -DOUT=<資料夾> -P voice/cmake/fetch_sources.cmake
#   cmake … -DNBTV_VOICE=ON -DNBTV_VOICE_SOURCES=<資料夾>
# 也可以用 FetchContent 本來就有的 -DFETCHCONTENT_SOURCE_DIR_RADE_C=、-DFETCHCONTENT_SOURCE_DIR_NBTV_OPUS= 指到已經解開的
# 原始碼目錄；那樣上面說的兩處會直接改在那個目錄裡，模型資料也解開在 Opus 的那個目錄裡。
#
# 一次建兩種處理器的版本（macOS 的 universal）不行：Opus 的建置一次只能一種，請分開建。

add_library(nbtvvoice STATIC src/voice.cpp)
add_library(nbtv::voice ALIAS nbtvvoice)
target_include_directories(nbtvvoice PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
target_compile_features(nbtvvoice PUBLIC cxx_std_20)
target_link_libraries(nbtvvoice PUBLIC nbtvcore)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nbtvvoice PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT NBTV_VOICE)
  target_compile_definitions(nbtvvoice PRIVATE NBTV_VOICE_RADE=0)
  set(NBTV_VOICE_SAMPLE "" CACHE INTERNAL "")
  return()
endif()

include(FetchContent)  # （C 語言在最上層的 CMakeLists.txt 開）

include(cmake/sources.cmake)
set(NBTV_VOICE_SOURCES "" CACHE PATH
    "Folder holding the three archives fetched beforehand by voice/cmake/fetch_sources.cmake; when empty they are downloaded at configure time")
set(NBTV_RADE_FROM "${NBTV_RADE_URL}")
set(NBTV_OPUS_FROM "${NBTV_OPUS_URL}")
set(NBTV_OPUS_MODEL_LOCAL "")
if(NBTV_VOICE_SOURCES)
  file(TO_CMAKE_PATH "${NBTV_VOICE_SOURCES}" NBTV_VOICE_SOURCES_DIR)
  foreach(file ${NBTV_RADE_FILE} ${NBTV_OPUS_FILE} ${NBTV_OPUS_MODEL_FILE})
    if(NOT EXISTS "${NBTV_VOICE_SOURCES_DIR}/${file}")
      message(FATAL_ERROR "NBTV_VOICE_SOURCES：${NBTV_VOICE_SOURCES_DIR} 裡沒有 ${file}（先執行 cmake -DOUT=<資料夾> -P voice/cmake/fetch_sources.cmake）")
    endif()
  endforeach()
  set(NBTV_RADE_FROM "${NBTV_VOICE_SOURCES_DIR}/${NBTV_RADE_FILE}")
  set(NBTV_OPUS_FROM "${NBTV_VOICE_SOURCES_DIR}/${NBTV_OPUS_FILE}")
  set(NBTV_OPUS_MODEL_LOCAL "${NBTV_VOICE_SOURCES_DIR}/${NBTV_OPUS_MODEL_FILE}")
endif()

# CMake 3.24 起下載的壓縮檔預設把解開的檔案時間設成「現在」，並且多了這個參數；較舊的版本不認得它
set(NBTV_VOICE_FETCH_OPTIONS "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
  set(NBTV_VOICE_FETCH_OPTIONS DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()

# ── 兩份原始碼：都只取下來，不在這裡用它們自己的 CMakeLists（rade_c 的假設自己是最上層的專案；Opus 的在下面另外加）
FetchContent_Declare(rade_c
  URL "${NBTV_RADE_FROM}"
  URL_HASH SHA256=${NBTV_RADE_SHA256}
  TLS_VERIFY ON
  ${NBTV_VOICE_FETCH_OPTIONS}
  SOURCE_SUBDIR not-built-with-its-own-cmake)
FetchContent_Declare(nbtv_opus
  URL "${NBTV_OPUS_FROM}"
  URL_HASH SHA256=${NBTV_OPUS_SHA256}
  TLS_VERIFY ON
  ${NBTV_VOICE_FETCH_OPTIONS}
  SOURCE_SUBDIR not-added-here)
FetchContent_MakeAvailable(rade_c nbtv_opus)
set(NBTV_RADE_SRC "${rade_c_SOURCE_DIR}/src")
set(NBTV_OPUS_SRC "${nbtv_opus_SOURCE_DIR}")
if(NOT EXISTS "${NBTV_RADE_SRC}/rade_api.h")
  message(FATAL_ERROR "在 ${rade_c_SOURCE_DIR} 找不到 rade_c 的原始碼")
endif()
if(NOT EXISTS "${NBTV_OPUS_SRC}/dnn/fargan.c")
  message(FATAL_ERROR "在 ${NBTV_OPUS_SRC} 找不到 Opus 的原始碼")
endif()

# ── Opus 的模型資料：權重是另外放的（原始碼的儲存庫裡沒有）。只解開用得到的：語音要的三組權重（.c），
# 以及每一組的標頭（Opus 別的原始檔會引用到，就算那一組沒有開）
set(NBTV_OPUS_DATA_FILES
  dnn/fargan_data.c dnn/pitchdnn_data.c dnn/plc_data.c
  dnn/fargan_data.h dnn/pitchdnn_data.h dnn/plc_data.h
  dnn/dred_rdovae_constants.h dnn/dred_rdovae_stats_data.h dnn/dred_rdovae_enc_data.h dnn/dred_rdovae_dec_data.h
  dnn/lace_data.h dnn/nolace_data.h dnn/bbwenet_data.h dnn/lossgen_data.h)
set(NBTV_OPUS_DATA_MISSING FALSE)
foreach(file ${NBTV_OPUS_DATA_FILES})
  if(NOT EXISTS "${NBTV_OPUS_SRC}/${file}")
    set(NBTV_OPUS_DATA_MISSING TRUE)
  endif()
endforeach()
if(NBTV_OPUS_DATA_MISSING)
  if(NBTV_OPUS_MODEL_LOCAL)
    set(NBTV_OPUS_MODEL_ARCHIVE "${NBTV_OPUS_MODEL_LOCAL}")
    file(SHA256 "${NBTV_OPUS_MODEL_ARCHIVE}" NBTV_OPUS_MODEL_GOT)
    if(NOT NBTV_OPUS_MODEL_GOT STREQUAL NBTV_OPUS_MODEL_SHA256)
      message(FATAL_ERROR "${NBTV_OPUS_MODEL_ARCHIVE} 不是預期的那一份模型資料：SHA-256 是 ${NBTV_OPUS_MODEL_GOT}，應該是 ${NBTV_OPUS_MODEL_SHA256}")
    endif()
  else()
    set(NBTV_OPUS_MODEL_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/${NBTV_OPUS_MODEL_FILE}")
    message(STATUS "語音的編解碼器：下載 Opus 的模型資料（約 184 MB）")
    file(DOWNLOAD "${NBTV_OPUS_MODEL_URL}" "${NBTV_OPUS_MODEL_ARCHIVE}"
      EXPECTED_HASH SHA256=${NBTV_OPUS_MODEL_SHA256} TLS_VERIFY ON STATUS NBTV_OPUS_MODEL_STATUS)
    list(GET NBTV_OPUS_MODEL_STATUS 0 NBTV_OPUS_MODEL_CODE)
    if(NOT NBTV_OPUS_MODEL_CODE EQUAL 0)
      message(FATAL_ERROR "Opus 的模型資料下載不下來：${NBTV_OPUS_MODEL_STATUS}（可以先用 voice/cmake/fetch_sources.cmake 抓好，再用 -DNBTV_VOICE_SOURCES= 指過去）")
    endif()
  endif()
  file(ARCHIVE_EXTRACT INPUT "${NBTV_OPUS_MODEL_ARCHIVE}" DESTINATION "${NBTV_OPUS_SRC}" PATTERNS ${NBTV_OPUS_DATA_FILES})
  foreach(file ${NBTV_OPUS_DATA_FILES})
    if(NOT EXISTS "${NBTV_OPUS_SRC}/${file}")
      message(FATAL_ERROR "Opus 的模型資料裡沒有 ${file}")
    endif()
  endforeach()
endif()

# ── rade_c 要求 Opus 改的地方：類神經網路層的輸入上限加大（RADE V2 的解碼器有一層要 1792 個輸入，Opus 自己最多用到 1536 個）。
# rade_c 是用補丁改的（src/opus-nnet.c.diff）；這裡直接改，Windows 上才不必另外找 patch。它的另一個補丁只是給動態函式庫
# 標匯出的符號，靜態連結用不到。
file(READ "${NBTV_OPUS_SRC}/dnn/nnet.c" NBTV_OPUS_NNET)
if(NOT NBTV_OPUS_NNET MATCHES "#define MAX_CONV_INPUTS_ALL 2048")
  string(REPLACE "#define MAX_CONV_INPUTS_ALL DRED_MAX_CONV_INPUTS" "#define MAX_CONV_INPUTS_ALL 2048" NBTV_OPUS_NNET_PATCHED "${NBTV_OPUS_NNET}")
  if(NBTV_OPUS_NNET_PATCHED STREQUAL NBTV_OPUS_NNET)
    message(FATAL_ERROR "Opus 的 dnn/nnet.c 裡找不到要改的那一行（版本不對？）")
  endif()
  file(WRITE "${NBTV_OPUS_SRC}/dnn/nnet.c" "${NBTV_OPUS_NNET_PATCHED}")
endif()

# ── rade_c 的 rade_api.h 在 Windows 上把每個函式標成 __declspec(dllexport／dllimport) __stdcall（給動態函式庫用的），
# 而且寫在回傳型別的前面。這裡是靜態連結，兩種標記都不對（執行檔會多出一批匯出的符號），也不想賭每一種編譯器都接受
# 那個寫法：在 Windows 上把它們拿掉。其他平台不定義 IS_BUILDING_RADE_API，那個巨集本來就是空的。
if(WIN32)
  file(READ "${NBTV_RADE_SRC}/rade_api.h" NBTV_RADE_API)
  string(REPLACE "#define RADE_EXPORT __declspec(dllexport) __stdcall" "#define RADE_EXPORT" NBTV_RADE_API_PATCHED "${NBTV_RADE_API}")
  string(REPLACE "#define RADE_EXPORT __declspec(dllimport) __stdcall" "#define RADE_EXPORT" NBTV_RADE_API_PATCHED "${NBTV_RADE_API_PATCHED}")
  if(NBTV_RADE_API_PATCHED MATCHES "__declspec")
    message(FATAL_ERROR "rade_c 的 rade_api.h 裡的匯出標記和預期的不一樣（版本不對？）")
  endif()
  if(NOT NBTV_RADE_API_PATCHED STREQUAL NBTV_RADE_API)
    file(WRITE "${NBTV_RADE_SRC}/rade_api.h" "${NBTV_RADE_API_PATCHED}")
  endif()
endif()

# ── Opus：用它自己的 CMakeLists 建成靜態函式庫。只開「deep PLC」那一組（裡面就有 LPCNet 的特徵與 FARGAN），
# 不開 DRED 與 OSCE（語音用不到，它們的權重也就不必連進來）。它的安裝規則、測試、命令列工具都不要。
set(OPUS_PACKAGE_VERSION "0.0.0")  # 不是 git 的工作目錄：自己給一個，免得它警告找不到版本
set(OPUS_DEEP_PLC ON)
# 浮點數的快速近似（log、exp 之類）：Opus 的 autotools 建置在 x86 與 ARM 上預設是開的，它的 CMakeLists 預設是關的。
# rade_c 自己用的是 autotools 的那一種；語音的特徵要算得和它一樣，所以開。
set(OPUS_FLOAT_APPROX ON)
set(BUILD_SHARED_LIBS OFF)
set(OPUS_BUILD_SHARED_LIBRARY OFF)
set(OPUS_BUILD_TESTING OFF)
set(OPUS_BUILD_PROGRAMS OFF)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)

# 做出來的程式要跑在哪一種處理器上。Opus 的 CMakeLists 看的是 CMAKE_SYSTEM_PROCESSOR，那是「建置的這台機器」的；
# 兩者可能不一樣的兩種情況在這裡算好，由 cmake/opus_project.cmake 在 Opus 的 project() 之後換上去。
set(NBTV_OPUS_PROCESSOR "")
if(MSVC)
  if(CMAKE_C_COMPILER_ARCHITECTURE_ID MATCHES "^ARM64")
    set(NBTV_OPUS_PROCESSOR "ARM64")
  elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    set(NBTV_OPUS_PROCESSOR "AMD64")
  elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "X86")
    set(NBTV_OPUS_PROCESSOR "x86")
  endif()
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
  if(CMAKE_OSX_ARCHITECTURES MATCHES ";")
    message(FATAL_ERROR "NBTV_VOICE：一次建兩種處理器的版本（CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}）不行，Opus 的建置一次只能一種；請分開建")
  endif()
  set(NBTV_OPUS_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
endif()
set(CMAKE_PROJECT_Opus_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/opus_project.cmake")
set(NBTV_OPUS_TARGET "${CMAKE_SYSTEM_PROCESSOR}")
if(NBTV_OPUS_PROCESSOR)
  set(NBTV_OPUS_TARGET "${NBTV_OPUS_PROCESSOR}")
endif()

# 64 位元的 ARM，Opus 的 CMakeLists 只在 Linux 上處理得對（執行時偵測，並且直接假設有 NEON）。另外兩種在這裡替它設：
#   * macOS（Apple 晶片）：它想在執行時偵測有沒有 NEON，但 macOS 上沒有偵測的寫法。Apple 晶片一定有，直接假設有，
#     不必偵測——和 Opus 的 autotools 建置在 macOS 上的結果一樣。它的 CMakeLists 在「假設有、不偵測」時漏了
#     NEON 那幾個函式的宣告要看的巨集，下面補上。
#   * Windows：它的執行時偵測是給 32 位元 ARM 寫的，64 位元的編不過。整個不用處理器專用的寫法，走一般的 C。
set(NBTV_OPUS_APPLE_ARM FALSE)
if(NBTV_OPUS_TARGET MATCHES "^(arm64|arm64e|aarch64|ARM64)$")
  if(APPLE)
    set(NBTV_OPUS_APPLE_ARM TRUE)
    set(OPUS_MAY_HAVE_NEON OFF)
    set(OPUS_PRESUME_NEON ON)
  elseif(WIN32)
    set(OPUS_DISABLE_INTRINSICS ON)
  endif()
endif()
message(STATUS "語音的編解碼器：rade_c ${NBTV_RADE_COMMIT}、Opus ${NBTV_OPUS_COMMIT}，處理器 ${NBTV_OPUS_TARGET}")

add_subdirectory("${NBTV_OPUS_SRC}" "${CMAKE_CURRENT_BINARY_DIR}/opus" EXCLUDE_FROM_ALL)
if(NBTV_OPUS_APPLE_ARM)
  target_compile_definitions(opus PRIVATE OPUS_ARM_MAY_HAVE_NEON OPUS_ARM_MAY_HAVE_NEON_INTR)
endif()
# Opus 的 CMakeLists 自己決定 C 執行階段怎麼連結（預設是動態的那一種）；要和專案其餘的部分一樣，不然連結時會衝突
if(MSVC AND DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set_property(TARGET opus PROPERTY MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

# ── RADE 的函式庫（靜態）與接它的那一層。原始碼的清單照 rade_c 的 src/CMakeLists.txt（librade 的那幾組）。
set(NBTV_RADE_SOURCES
  rade_api.c rade_enc.c rade_dec.c rade_enc_data.c rade_dec_data.c
  rade_dsp.c rade_ofdm.c rade_bpf.c rade_acq.c rade_tx.c rade_rx.c
  rade_enc_v2.c rade_dec_v2.c rade_sync.c rade_enc_v2_data.c rade_dec_v2_data.c rade_sync_data.c
  rade_v2_ofdm.c rade_tx_v2.c rade_rx_v2.c)
list(TRANSFORM NBTV_RADE_SOURCES PREPEND "${NBTV_RADE_SRC}/")
add_library(nbtvrade STATIC ${NBTV_RADE_SOURCES} src/rade_glue.c)
set_target_properties(nbtvrade PROPERTIES C_STANDARD 11)
# Opus 內部的標頭（lpcnet.h、fargan.h、nnet.h…）不在它對外的 include/ 裡
target_include_directories(nbtvrade PRIVATE "${NBTV_RADE_SRC}" "${NBTV_OPUS_SRC}/dnn" "${NBTV_OPUS_SRC}/celt" "${NBTV_OPUS_SRC}/include" "${NBTV_OPUS_SRC}" src)
if(MSVC)
  target_compile_definitions(nbtvrade PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
  target_compile_options(nbtvrade PRIVATE /w)   # 別人的程式：不看它的警告
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nbtvrade PRIVATE -w)
endif()
target_link_libraries(nbtvrade PRIVATE opus)
if(UNIX AND NOT APPLE)
  target_link_libraries(nbtvrade PRIVATE m)
endif()

string(SUBSTRING "${NBTV_RADE_COMMIT}" 0 7 NBTV_RADE_SHORT)
target_compile_definitions(nbtvvoice PRIVATE NBTV_VOICE_RADE=1 NBTV_VOICE_RADE_VERSION="${NBTV_RADE_SHORT}")
target_include_directories(nbtvvoice PRIVATE src)
target_link_libraries(nbtvvoice PRIVATE nbtvrade)

# 測試用的語音：rade_c 附的一小段（16 kHz）
set(NBTV_VOICE_SAMPLE "${rade_c_SOURCE_DIR}/input_sample.wav" CACHE INTERNAL "")

# ── 這兩份別人的程式連進了執行檔：散佈時要附上它們的授權條文（LICENSES/）。
# macOS 的 app 放在它的 Resources 裡，桌面 app 併進它的 THIRD-PARTY-LICENSES.txt（用下面這一段文字）；
# 沒有建桌面 app、只有命令列工具（nbtv-voice）的安裝，在這裡做一份同名的檔案。
file(READ LICENSES/rade_c-LICENSE.txt NBTV_RADE_LICENSE)
file(READ LICENSES/rade_c-source-notice.txt NBTV_RADE_NOTICE)
file(READ LICENSES/opus-COPYING.txt NBTV_OPUS_LICENSE)
set(NBTV_VOICE_NOTICES "\n================================================================\nrade_c (RADE, the Radio Autoencoder, and its weights) - https://github.com/freedv/rade_c\n================================================================\n\n${NBTV_RADE_LICENSE}\nThe individual source files carry this notice (the year differs from file to file):\n\n${NBTV_RADE_NOTICE}")
string(APPEND NBTV_VOICE_NOTICES "\n================================================================\nOpus (LPCNet features and the FARGAN vocoder) - https://github.com/xiph/opus\n================================================================\n\n${NBTV_OPUS_LICENSE}")
set(NBTV_VOICE_NOTICES "${NBTV_VOICE_NOTICES}" PARENT_SCOPE)
if(NBTV_BUILD_TOOLS AND NOT NBTV_BUILD_DESKTOP_APP)
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-LICENSES.txt" @ONLY
    CONTENT "nbtv-voice (the voice codec tool of NBeeTv) is statically linked with the following third-party software.\nNBeeTv itself is licensed under the GNU General Public License v3.0 (see LICENSE).\n${NBTV_VOICE_NOTICES}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-LICENSES.txt" DESTINATION "${NBTV_INSTALL_DOCDIR}")
endif()
