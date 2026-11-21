cmake_minimum_required(VERSION 3.20)
project(hf_nbtv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NBTV_BUILD_TESTS "Build the unit tests" ON)
option(NBTV_BUILD_TOOLS "Build the command line tools" ON)
option(NBTV_WITH_AVIF "Use libavif (libaom, dav1d) for the digital mode's image codec" ON)
option(NBTV_BUILD_MACOS_APP "Build the macOS app (SwiftUI shell)" OFF)
option(NBTV_MACOS_SECOND_APP "Also build NBTV 2.app, a second copy with its own settings, to run two at once" OFF)

add_subdirectory(core)
add_subdirectory(sim)

if(NBTV_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(NBTV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
endif()

if(APPLE AND NBTV_BUILD_MACOS_APP)
  add_subdirectory(apps/macos)
endif()
