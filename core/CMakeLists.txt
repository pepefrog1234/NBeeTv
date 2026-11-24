# 平台無關的核心函式庫：不開執行緒、不碰音訊裝置，對外只有 include/nbtv/nbtv.h 的 C API。
add_library(nbtvcore STATIC
  src/bits.cpp
  src/c_api.cpp
  src/callsign.cpp
  src/chanest.cpp
  src/conv_code.cpp
  src/dsp.cpp
  src/fft.cpp
  src/frame_header.cpp
  src/frame_map.cpp
  src/learned_codec.cpp
  src/learned_frame.cpp
  src/learned_merge.cpp
  src/ofdm_tx.cpp
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
