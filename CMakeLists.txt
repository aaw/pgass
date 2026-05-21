cmake_minimum_required(VERSION 3.14)
project(ParseProject LANGUAGES CXX)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# 1. Fetch GoogleTest and Abseil from GitHub automatically
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
)
FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG        20260107.1
)

# Optimization: Don't build all of GTest/Abseil, just what we need
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest abseil-cpp)

add_library(parse_lib INTERFACE)
target_include_directories(parse_lib INTERFACE src)
target_link_libraries(parse_lib
  INTERFACE
    absl::strings
    absl::statusor
)

add_library(safety_lib STATIC src/safety.cc)
target_compile_options(safety_lib PRIVATE -Wall -Wextra -Werror)
target_link_libraries(safety_lib
  PUBLIC
    parse_lib
  PRIVATE
    absl::check
    absl::flat_hash_set
)

add_executable(pgass src/pgass.cc)
target_compile_options(pgass PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pgass
  PRIVATE
    safety_lib
    absl::check
    absl::flags
    absl::flags_parse
    absl::flags_usage
)

add_executable(parse_test src/parse_test.cc)
target_compile_options(parse_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(parse_test
  PRIVATE
    parse_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(safety_test src/safety_test.cc)
target_compile_options(safety_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(safety_test
  PRIVATE
    safety_lib
    GTest::gtest_main
    GTest::gmock
)

include(GoogleTest)
gtest_discover_tests(parse_test)
gtest_discover_tests(safety_test)
