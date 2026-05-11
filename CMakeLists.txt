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

add_library(parse_lib parse.cc parse.h tokenize.h)

target_link_libraries(parse_lib
  PUBLIC
    absl::strings
    absl::statusor
)

add_executable(parse_test parse_test.cc)

target_link_libraries(parse_test
  PRIVATE
    parse_lib
    GTest::gtest_main
    GTest::gmock
)

include(GoogleTest)
gtest_discover_tests(parse_test)
