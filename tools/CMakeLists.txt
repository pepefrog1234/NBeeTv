# 命令列工具。工具可以直接使用核心內部的標頭（core/src）。
function(nbtv_add_tool name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE nbtvcore)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  # 各個模式的權重檔：執行檔旁邊沒有的時候從這裡找（tools/model_file.hpp）
  target_compile_definitions(${name} PRIVATE NBTV_MODEL_DIR="${PROJECT_SOURCE_DIR}/models")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(MSVC)
    # 命令列參數與檔名一律當成 UTF-8，路徑裡有中文也開得了（說明在那個檔案裡）
    target_sources(${name} PRIVATE ${PROJECT_SOURCE_DIR}/tools/windows/utf8.manifest)
  endif()
  if(NBTV_STATIC_TOOLS AND NOT MSVC AND NOT APPLE)
    # 整個靜態連結：不挑發行版與 glibc 的版本（GitHub 上 Linux 的下載包就是這樣做的）。
    # 只有這幾個工具這樣連；桌面 app 要在執行時載入系統的視窗、音訊函式庫，不能靜態連結。
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

nbtv_add_tool(nbtv-tx nbtv_tx.cpp)
nbtv_add_tool(nbtv-rx nbtv_rx.cpp)
nbtv_add_tool(nbtv-chan nbtv_chan.cpp)
target_link_libraries(nbtv-chan PRIVATE nbtvsim)
nbtv_add_tool(nbtv-sim nbtv_sim.cpp)
target_link_libraries(nbtv-sim PRIVATE nbtvsim)
nbtv_add_tool(nbtv-learned nbtv_learned.cpp)

install(TARGETS nbtv-tx nbtv-rx nbtv-chan nbtv-sim nbtv-learned RUNTIME DESTINATION "${NBTV_INSTALL_BINDIR}")

# 設計工具：產生寫進核心的常數，平常不需要執行。
nbtv_add_tool(design_preamble design/design_preamble.cpp)
set_target_properties(design_preamble PROPERTIES EXCLUDE_FROM_ALL ON)
