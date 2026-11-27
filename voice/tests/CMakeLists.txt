# 語音模組的測試。沒有把編解碼器編進來時（預設），只檢查空殼的行為。
find_package(Threads REQUIRED)
add_executable(test_voice test_voice.cpp)
target_link_libraries(test_voice PRIVATE nbtvvoice nbtvcore nbtvsim Threads::Threads)
target_include_directories(test_voice PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/tests ${PROJECT_SOURCE_DIR}/tools)
target_compile_definitions(test_voice PRIVATE
  NBTV_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/core/tests/data"
  NBTV_MODEL_DIR="${PROJECT_SOURCE_DIR}/models"
  NBTV_VOICE_SAMPLE="${NBTV_VOICE_SAMPLE}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(test_voice PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME voice COMMAND test_voice)
