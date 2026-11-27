# voice/CMakeLists.txt 用 CMAKE_PROJECT_Opus_INCLUDE 把這個檔掛在 Opus 的 project() 之後執行（在 Opus 自己的目錄範圍裡）。
#
# Opus 的 CMakeLists 靠 CMAKE_SYSTEM_PROCESSOR 決定要編哪一種處理器的程式（x86 的 SSE／AVX2，或 ARM 的 NEON）。
# 那個變數是「建置的這台機器」的處理器，不一定是做出來的程式要跑的那一種：
#   * Visual Studio 的產生器用 -A 選處理器，變數不跟著變；
#   * macOS 用 CMAKE_OSX_ARCHITECTURES 選處理器，變數也不跟著變。
# 而且每一次 project() 都會把它設回去，所以只能在這裡改。要建的是哪一種由 voice/CMakeLists.txt 算好放在 NBTV_OPUS_PROCESSOR。
if(NBTV_OPUS_PROCESSOR)
  set(CMAKE_SYSTEM_PROCESSOR "${NBTV_OPUS_PROCESSOR}")
endif()
