# 平台無關的核心函式庫：不開執行緒、不碰音訊裝置，對外只有 include/nbtv/nbtv.h 的 C API。
add_library(nbtvcore STATIC
  src/analog_frame.cpp
  src/analog_tables.cpp
  src/av1_payload.cpp
  src/bit_interleaver.cpp
  src/bits.cpp
  src/c_api.cpp
  src/callsign.cpp
  src/chanest.cpp
  src/conv_code.cpp
  src/dct.cpp
  src/digital_frame.cpp
  src/dsp.cpp
  src/fft.cpp
  src/frame_map.cpp
  src/image_codec.cpp
  src/ldpc.cpp
  src/ofdm_tx.cpp
  src/qam.cpp
  src/receiver.cpp
  src/spectrum.cpp
  src/transmitter.cpp
)
add_library(nbtv::core ALIAS nbtvcore)

target_include_directories(nbtvcore
  PUBLIC include
  PRIVATE src
)
target_compile_features(nbtvcore PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nbtvcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 數位模式的影像編解碼需要 libavif（編碼 libaom、解碼 dav1d 或 libaom）。
# 找不到時核心照樣可以建置：數據機與相干類比都能用，只是影像的編解碼要由外殼自己處理。
set(NBTV_HAVE_AVIF OFF)
if(NBTV_WITH_AVIF)
  find_package(libavif CONFIG QUIET)
  if(TARGET avif)
    target_link_libraries(nbtvcore PRIVATE avif)
    set(NBTV_HAVE_AVIF ON)
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(NBTV_AVIF QUIET IMPORTED_TARGET libavif)
      if(NBTV_AVIF_FOUND)
        target_link_libraries(nbtvcore PRIVATE PkgConfig::NBTV_AVIF)
        set(NBTV_HAVE_AVIF ON)
      endif()
    endif()
  endif()
endif()
if(NBTV_HAVE_AVIF)
  target_compile_definitions(nbtvcore PRIVATE NBTV_HAVE_AVIF=1)
  message(STATUS "hf-nbtv: 影像編解碼使用 libavif")
else()
  target_compile_definitions(nbtvcore PRIVATE NBTV_HAVE_AVIF=0)
  if(NBTV_WITH_AVIF)
    message(STATUS "hf-nbtv: 找不到 libavif，核心不含影像編解碼")
  else()
    message(STATUS "hf-nbtv: NBTV_WITH_AVIF 已關閉，核心不含影像編解碼")
  endif()
endif()
set(NBTV_HAVE_AVIF ${NBTV_HAVE_AVIF} PARENT_SCOPE)
