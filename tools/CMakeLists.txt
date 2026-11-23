# 命令列工具。工具可以直接使用核心內部的標頭（core/src）。
function(nbtv_add_tool name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE nbtvcore)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  # 各個模式的權重檔預設從這裡找
  target_compile_definitions(${name} PRIVATE NBTV_MODEL_DIR="${PROJECT_SOURCE_DIR}/models")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

nbtv_add_tool(nbtv-tx nbtv_tx.cpp)
nbtv_add_tool(nbtv-rx nbtv_rx.cpp)
nbtv_add_tool(nbtv-chan nbtv_chan.cpp)
target_link_libraries(nbtv-chan PRIVATE nbtvsim)
nbtv_add_tool(nbtv-sim nbtv_sim.cpp)
target_link_libraries(nbtv-sim PRIVATE nbtvsim)
nbtv_add_tool(nbtv-learned nbtv_learned.cpp)

# 設計工具：產生寫進核心的常數，平常不需要執行。
nbtv_add_tool(design_preamble design/design_preamble.cpp)
set_target_properties(design_preamble PROPERTIES EXCLUDE_FROM_ALL ON)
