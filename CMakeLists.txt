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

add_library(parse_lib STATIC src/parse.cc src/ast.cc)
target_compile_options(parse_lib PRIVATE -Wall -Wextra -Werror)
target_include_directories(parse_lib PUBLIC src)
target_link_libraries(parse_lib
  PUBLIC
    absl::statusor
  PRIVATE
    absl::strings
)

add_library(collect_lib STATIC src/collect.cc)
target_compile_options(collect_lib PRIVATE -Wall -Wextra -Werror)
target_link_libraries(collect_lib
  PUBLIC
    parse_lib
    absl::flat_hash_set
)

add_library(safety_lib STATIC src/safety.cc)
target_compile_options(safety_lib PRIVATE -Wall -Wextra -Werror)
target_link_libraries(safety_lib
  PUBLIC
    parse_lib
  PRIVATE
    absl::check
    absl::flat_hash_set
    collect_lib
    graph_lib
)

add_library(graph_lib STATIC src/graph.cc)
target_compile_options(graph_lib PRIVATE -Wall -Wextra -Werror)
target_link_libraries(graph_lib
  PUBLIC
    parse_lib
    absl::flat_hash_map
  PRIVATE
    collect_lib
)

add_library(normalize_lib STATIC src/normalize.cc)
target_compile_options(normalize_lib PRIVATE -Wall -Wextra -Werror)
target_link_libraries(normalize_lib
  PUBLIC
    parse_lib
  PRIVATE
    absl::btree
    absl::check
    absl::flat_hash_map
    absl::flat_hash_set
    collect_lib
    graph_lib
)

add_library(aspif_lib STATIC src/aspif.cc)
target_compile_options(aspif_lib PRIVATE -Wall -Wextra -Werror)
target_include_directories(aspif_lib PUBLIC src)
target_link_libraries(aspif_lib
  PRIVATE
    absl::strings
)

add_library(ground_lib STATIC src/ground.cc)
target_compile_options(ground_lib PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ground_lib
  PUBLIC
    aspif_lib
    parse_lib
    absl::statusor
  PRIVATE
    absl::flat_hash_map
    absl::strings
    graph_lib
)

add_library(format_lib STATIC src/format.cc)
target_compile_options(format_lib PRIVATE -Wall -Wextra -Werror)
target_link_libraries(format_lib
  PUBLIC
    parse_lib
)

add_executable(pgass src/pgass.cc)
target_compile_options(pgass PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pgass
  PRIVATE
    format_lib
    ground_lib
    normalize_lib
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

add_executable(normalize_test src/normalize_test.cc)
target_compile_options(normalize_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(normalize_test
  PRIVATE
    format_lib
    normalize_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(collect_test src/collect_test.cc)
target_compile_options(collect_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(collect_test
  PRIVATE
    collect_lib
    parse_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(aspif_test src/aspif_test.cc)
target_compile_options(aspif_test PRIVATE
  -Wall -Wextra -Werror -Wno-missing-field-initializers)
target_link_libraries(aspif_test
  PRIVATE
    aspif_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(ground_test src/ground_test.cc)
target_compile_options(ground_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ground_test
  PRIVATE
    ground_lib
    normalize_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(graph_test src/graph_test.cc)
target_compile_options(graph_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(graph_test
  PRIVATE
    graph_lib
    parse_lib
    GTest::gtest_main
    GTest::gmock
)

include(GoogleTest)
gtest_discover_tests(parse_test)
gtest_discover_tests(safety_test)
gtest_discover_tests(normalize_test)
gtest_discover_tests(collect_test)
gtest_discover_tests(graph_test)
gtest_discover_tests(aspif_test)
gtest_discover_tests(ground_test)
