# 測試可以直接看核心內部的標頭（core/src）。
# 執行緒：test_link 把可能卡住的接收機放在另一條執行緒跑，等不到就算失敗。
find_package(Threads REQUIRED)

function(nbtv_add_test name)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE nbtvcore nbtvsim Threads::Threads)
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
nbtv_add_test(kernels)
nbtv_add_test(learned)
nbtv_add_test(merge)
nbtv_add_test(link)
nbtv_add_test(c_api)

# 推論的內層迴圈有兩種寫法（core/src/learned_kernels.hpp），平常依 CPU 自動選：x86-64 上有 AVX2 的電腦用 avx2 那一種。
# 所以 x86-64 上另外指定用一般的寫法（沒有 AVX2 的電腦走的那條路）各跑一次：零件的測試只用一條執行緒，
# 和 PyTorch 對照的那一項照預設的執行緒數（只用一條的話要好幾分鐘）。別的處理器上只有一種寫法，不必重跑。
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  add_test(NAME kernels_generic COMMAND test_kernels)
  set_tests_properties(kernels_generic PROPERTIES ENVIRONMENT "NBTV_KERNEL=generic;NBTV_THREADS=1")
  add_test(NAME learned_generic COMMAND test_learned)
  set_tests_properties(learned_generic PROPERTIES ENVIRONMENT "NBTV_KERNEL=generic")
endif()
