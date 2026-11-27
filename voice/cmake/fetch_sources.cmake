# 先把語音的編解碼器要的三份檔案抓到一個資料夾（核對 SHA-256），建置時用 -DNBTV_VOICE_SOURCES=<資料夾> 指過去，
# 設定（configure）時就不必再連網路。CI 就是這樣做的：每次執行抓一次，各個平台的建置共用。
#
#   cmake -DOUT=<資料夾> -P voice/cmake/fetch_sources.cmake
#
# 已經在資料夾裡、內容也對的檔案不會重抓。版本、網址與雜湊值在 sources.cmake。
cmake_minimum_required(VERSION 3.20)

if(NOT OUT)
  message(FATAL_ERROR "用法：cmake -DOUT=<資料夾> -P voice/cmake/fetch_sources.cmake")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")
get_filename_component(NBTV_FETCH_DIR "${OUT}" ABSOLUTE)
file(MAKE_DIRECTORY "${NBTV_FETCH_DIR}")

foreach(item RADE OPUS OPUS_MODEL)
  set(path "${NBTV_FETCH_DIR}/${NBTV_${item}_FILE}")
  if(EXISTS "${path}")
    file(SHA256 "${path}" have)
    if(have STREQUAL NBTV_${item}_SHA256)
      message(STATUS "已經有了：${NBTV_${item}_FILE}")
      continue()
    endif()
    file(REMOVE "${path}")
  endif()
  message(STATUS "下載 ${NBTV_${item}_URL}")
  file(DOWNLOAD "${NBTV_${item}_URL}" "${path}" TLS_VERIFY ON STATUS status)
  list(GET status 0 code)
  if(code EQUAL 0)
    file(SHA256 "${path}" have)
    if(NOT have STREQUAL NBTV_${item}_SHA256)
      file(REMOVE "${path}")
      message(FATAL_ERROR "${NBTV_${item}_URL} 的內容不是預期的那一份：SHA-256 是 ${have}，應該是 ${NBTV_${item}_SHA256}")
    endif()
  else()
    file(REMOVE "${path}")
    message(FATAL_ERROR "下載不下來：${NBTV_${item}_URL}（${status}）")
  endif()
endforeach()

message(STATUS "三份都在 ${NBTV_FETCH_DIR}")
message(STATUS "建置時加上：-DNBTV_VOICE=ON -DNBTV_VOICE_SOURCES=${NBTV_FETCH_DIR}")
