# 通道模擬：測試與模擬工具共用，不屬於核心函式庫。
add_library(nbtvsim STATIC channel.cpp)
target_include_directories(nbtvsim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/core/src)
target_compile_features(nbtvsim PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nbtvsim PRIVATE -Wall -Wextra -Wpedantic)
endif()
