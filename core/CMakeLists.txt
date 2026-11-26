# 平台無關的核心函式庫：不碰音訊裝置，對外只有 include/nbtv/nbtv.h 的 C API。
# 只有類神經網路的推論會開執行緒（src/parallel.hpp）：一張畫面的編碼、解碼分給幾條執行緒一起算。
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
  src/learned_kernels.cpp
  src/learned_kernels_avx2.cpp
  src/learned_merge.cpp
  src/ofdm_tx.cpp
  src/parallel.cpp
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

find_package(Threads REQUIRED)
target_link_libraries(nbtvcore PUBLIC Threads::Threads)

# x86-64 的 MSVC：推論內層迴圈用 AVX2 寫的那一份（src/learned_kernels_avx2.cpp）整個檔案照 AVX2 編譯。
# 那個檔案裡的程式只有在確認過 CPU 與作業系統都支援之後才會被呼叫，而且刻意不含會和別的檔案共用的行內函式（原因寫在它的開頭）。
# GCC 與 Clang 不需要這個選項：要用的指令集是在原始碼裡一個函式一個函式標的。別的處理器上那個檔案是空的。
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  set_source_files_properties(src/learned_kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nbtvcore PRIVATE -Wall -Wextra -Wpedantic)
endif()
