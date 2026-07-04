cmake_minimum_required(VERSION 3.20)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build so the benchmark is meaningful out of the box.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Core library -----------------------------------------------------------
add_library(ob STATIC
  src/order_book.cpp
  src/itch_parser.cpp)
target_include_directories(ob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(ob PRIVATE -Wall -Wextra -Wpedantic)

# --- Tests ------------------------------------------------------------------
enable_testing()
add_executable(ob_tests
  tests/main.cpp
  tests/test_matching.cpp
  tests/test_itch.cpp)
target_link_libraries(ob_tests PRIVATE ob)
target_include_directories(ob_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME ob_tests COMMAND ob_tests)

# --- Benchmark --------------------------------------------------------------
add_executable(ob_bench bench/bench_latency.cpp)
target_link_libraries(ob_bench PRIVATE ob)
target_compile_options(ob_bench PRIVATE -O3)

# --- ITCH replay tool -------------------------------------------------------
add_executable(itch_replay apps/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE ob)
