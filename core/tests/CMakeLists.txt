# 測試可以直接看核心內部的標頭（core/src）。
function(nbtv_add_test name)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE nbtvcore nbtvsim)
  target_include_directories(test_${name} PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/tools)
  target_compile_definitions(test_${name} PRIVATE
    NBTV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    NBTV_MODEL_DIR="${PROJECT_SOURCE_DIR}/models")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

nbtv_add_test(modes)
nbtv_add_test(bits)
nbtv_add_test(frame)
nbtv_add_test(learned)
nbtv_add_test(link)
nbtv_add_test(c_api)
