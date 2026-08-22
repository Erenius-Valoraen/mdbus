cmake_minimum_required(VERSION 3.16)
project(mdbus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Benchmarks are meaningless in a debug build; default to Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

find_package(Threads REQUIRED)

# -march=native: this is a benchmark of *this* machine, not a portable artifact.
add_library(mdbus_flags INTERFACE)
target_compile_options(mdbus_flags INTERFACE
  -Wall -Wextra
  $<$<CONFIG:Release>:-O3;-march=native>)

# The library itself is header-only; populated from step S1 onwards.
add_library(mdbus INTERFACE)
target_include_directories(mdbus INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mdbus INTERFACE Threads::Threads mdbus_flags)

# The original 2-core ping-pong this project grew out of. Kept buildable so the
# "atom" of the bus stays runnable as a reference point.
add_executable(pingpong_legacy legacy/pingpong.cpp)
target_link_libraries(pingpong_legacy PRIVATE mdbus)

enable_testing()
