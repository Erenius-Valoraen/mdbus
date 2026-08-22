cmake_minimum_required(VERSION 3.16)
# NOTE: the product name lives here and in the docs only. All code uses the
# neutral `bus` include prefix and namespace, so renaming the project is a
# one-line change rather than a repo-wide sed.
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
add_library(bus_flags INTERFACE)
target_compile_options(bus_flags INTERFACE
  -Wall -Wextra
  $<$<CONFIG:Release>:-O3;-march=native>)

# The library itself is header-only; populated from step S1 onwards.
add_library(bus INTERFACE)
target_include_directories(bus INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bus INTERFACE Threads::Threads bus_flags)

# The original 2-core ping-pong this project grew out of. Kept buildable so the
# "atom" of the bus stays runnable as a reference point.
add_executable(pingpong_legacy legacy/pingpong.cpp)
target_link_libraries(pingpong_legacy PRIVATE bus)

enable_testing()

# Tests. Each is a standalone binary returning non-zero on failure, registered
# with ctest. add_test's name is what `ctest -R` matches on.
add_executable(test_timing tests/test_timing.cpp)
target_link_libraries(test_timing PRIVATE bus)
add_test(NAME timing COMMAND test_timing)
