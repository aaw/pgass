cmake_minimum_required(VERSION 3.15)
project(ParseProject LANGUAGES CXX)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Needed on Linux so these static libs can link into the _native shared object.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# An unset build type means no optimization flags at all, which costs grounding
# a factor of about 35, so `make` gets an optimized build with debug info by
# default. `make asan` configures its own build directory as Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

enable_testing()

# The warning flags every target in this project builds with.
# -Wmissing-field-initializers is off: compilers disagree on when it fires.
set(PGASS_WARNINGS -Wall -Wextra -Werror -Wno-missing-field-initializers)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 14 misfires this on absl::InlinedVector's internal move. Clang lacks the flag.
  list(APPEND PGASS_WARNINGS -Wno-maybe-uninitialized)
endif()

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

# cvc5, the SMT solver the QF_IDL translation is handed to.
include(cmake/cvc5.cmake)

add_library(bigint_lib STATIC src/bigint.cc)
target_compile_options(bigint_lib PRIVATE ${PGASS_WARNINGS})
target_include_directories(bigint_lib PUBLIC src)
target_link_libraries(bigint_lib
  PUBLIC
    absl::inlined_vector
  PRIVATE
    absl::str_format
    absl::strings
)

add_library(parse_lib STATIC src/parse.cc src/ast.cc)
target_compile_options(parse_lib PRIVATE ${PGASS_WARNINGS})
target_include_directories(parse_lib PUBLIC src)
target_link_libraries(parse_lib
  PUBLIC
    bigint_lib
    absl::statusor
  PRIVATE
    absl::strings
)

add_library(collect_lib STATIC src/collect.cc)
target_compile_options(collect_lib PRIVATE ${PGASS_WARNINGS})
target_link_libraries(collect_lib
  PUBLIC
    parse_lib
    absl::flat_hash_set
)

add_library(safety_lib STATIC src/safety.cc)
target_compile_options(safety_lib PRIVATE ${PGASS_WARNINGS})
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
target_compile_options(graph_lib PRIVATE ${PGASS_WARNINGS})
target_link_libraries(graph_lib
  PUBLIC
    parse_lib
    absl::flat_hash_map
  PRIVATE
    collect_lib
)

add_library(normalize_lib STATIC src/normalize.cc)
target_compile_options(normalize_lib PRIVATE ${PGASS_WARNINGS})
target_link_libraries(normalize_lib
  PUBLIC
    parse_lib
  PRIVATE
    absl::btree
    collect_lib
)

add_library(aspif_lib STATIC src/aspif.cc)
target_compile_options(aspif_lib PRIVATE ${PGASS_WARNINGS})
target_include_directories(aspif_lib PUBLIC src)
target_link_libraries(aspif_lib
  PUBLIC
    bigint_lib
    absl::statusor
  PRIVATE
    absl::strings
)

add_library(symbols_lib STATIC src/symbols.cc)
target_compile_options(symbols_lib PRIVATE ${PGASS_WARNINGS})
target_include_directories(symbols_lib PUBLIC src)
target_link_libraries(symbols_lib
  PUBLIC
    bigint_lib
    absl::node_hash_map
    absl::span
  PRIVATE
    absl::strings
)

add_library(ground_lib STATIC src/ground.cc)
target_compile_options(ground_lib PRIVATE ${PGASS_WARNINGS})
target_link_libraries(ground_lib
  PUBLIC
    aspif_lib
    parse_lib
    absl::statusor
  PRIVATE
    absl::btree
    absl::check
    absl::flat_hash_map
    absl::flat_hash_set
    absl::inlined_vector
    absl::strings
    collect_lib
    format_lib
    graph_lib
    symbols_lib
)

add_library(format_lib STATIC src/format.cc)
target_compile_options(format_lib PRIVATE ${PGASS_WARNINGS})
target_link_libraries(format_lib
  PUBLIC
    parse_lib
)

# The translation of a ground program into SMT, which solve_lib searches and
# encode_smtlib() prints.
add_library(encode_lib STATIC src/encode.cc)
target_compile_options(encode_lib PRIVATE ${PGASS_WARNINGS})
target_include_directories(encode_lib PUBLIC src)
# PUBLIC because cvc5's dependencies reach the final link line as bare names;
# see cmake/cvc5.cmake.
target_link_directories(encode_lib PUBLIC ${PGASS_CVC5_LIB_DIR})
target_link_libraries(encode_lib
  PUBLIC
    aspif_lib
    absl::statusor
    # PUBLIC because encode.h holds cvc5 terms, so whoever includes it needs
    # cvc5's headers.
    cvc5::cvc5
  PRIVATE
    absl::check
    absl::flat_hash_set
    absl::strings
    graph_lib
)

add_library(solve_lib STATIC src/solve.cc)
target_compile_options(solve_lib PRIVATE ${PGASS_WARNINGS})
target_include_directories(solve_lib PUBLIC src)
target_link_libraries(solve_lib
  PUBLIC
    aspif_lib
    encode_lib
    absl::statusor
  PRIVATE
    absl::btree
    absl::check
    absl::flat_hash_set
    absl::strings
    cvc5::cvc5
)

add_executable(pgass src/pgass.cc src/platform.cc)
target_compile_options(pgass PRIVATE ${PGASS_WARNINGS})
target_link_libraries(pgass
  PRIVATE
    encode_lib
    format_lib
    ground_lib
    normalize_lib
    safety_lib
    solve_lib
    absl::check
    absl::flat_hash_set
    absl::flags
    absl::flags_parse
    absl::flags_usage
)

add_executable(bigint_test src/bigint_test.cc)
target_compile_options(bigint_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(bigint_test
  PRIVATE
    bigint_lib
    absl::hash
    absl::strings
    GTest::gtest_main
    GTest::gmock
)

add_executable(parse_test src/parse_test.cc)
target_compile_options(parse_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(parse_test
  PRIVATE
    parse_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(safety_test src/safety_test.cc)
target_compile_options(safety_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(safety_test
  PRIVATE
    safety_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(format_test src/format_test.cc)
target_compile_options(format_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(format_test
  PRIVATE
    format_lib
    parse_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(normalize_test src/normalize_test.cc)
target_compile_options(normalize_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(normalize_test
  PRIVATE
    format_lib
    normalize_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(collect_test src/collect_test.cc)
target_compile_options(collect_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(collect_test
  PRIVATE
    collect_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(aspif_test src/aspif_test.cc)
target_compile_options(aspif_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(aspif_test
  PRIVATE
    aspif_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(ground_test src/ground_test.cc)
target_compile_options(ground_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(ground_test
  PRIVATE
    ground_lib
    normalize_lib
    GTest::gtest_main
    GTest::gmock
)

add_executable(solve_test src/solve_test.cc)
target_compile_options(solve_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(solve_test
  PRIVATE
    ground_lib
    normalize_lib
    solve_lib
    GTest::gtest_main
    GTest::gmock
)

# Links cvc5's parser to read the printed script back and check it is really
# SMT-LIB. encode_lib publishes cvc5 itself, along with the link directory the
# bare dependency names need.
add_executable(encode_test src/encode_test.cc)
target_compile_options(encode_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(encode_test
  PRIVATE
    encode_lib
    ground_lib
    normalize_lib
    absl::strings
    cvc5::cvc5parser
    GTest::gtest_main
    GTest::gmock
)

add_executable(graph_test src/graph_test.cc)
target_compile_options(graph_test PRIVATE ${PGASS_WARNINGS})
target_link_libraries(graph_test
  PRIVATE
    graph_lib
    GTest::gtest_main
    GTest::gmock
)

# Runs the built binary to check the exit codes it returns, so it needs to know
# where that binary and the examples it solves ended up.
add_executable(exit_code_test src/exit_code_test.cc)
target_compile_options(exit_code_test PRIVATE ${PGASS_WARNINGS})
target_compile_definitions(exit_code_test
  PRIVATE
    PGASS_BINARY="$<TARGET_FILE:pgass>"
    PGASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_dependencies(exit_code_test pgass)
target_link_libraries(exit_code_test
  PRIVATE
    absl::strings
    GTest::gtest_main
    GTest::gmock
)

include(GoogleTest)
gtest_discover_tests(bigint_test)
gtest_discover_tests(parse_test)
gtest_discover_tests(safety_test)
gtest_discover_tests(format_test)
gtest_discover_tests(normalize_test)
gtest_discover_tests(collect_test)
gtest_discover_tests(graph_test)
gtest_discover_tests(aspif_test)
gtest_discover_tests(ground_test)
gtest_discover_tests(solve_test)
gtest_discover_tests(encode_test)
gtest_discover_tests(exit_code_test)

# Built only when PGASS_BUILD_PYTHON is set. scikit-build-core turns it on
# when it invokes cmake to build the wheel. See pyproject.toml.
option(PGASS_BUILD_PYTHON "Build the pgass Python extension module" OFF)
if(PGASS_BUILD_PYTHON)
  # nanobind over pybind11: STABLE_ABI lets one wheel (built against the
  # 3.12 limited API) cover every Python from 3.12 up, so cibuildwheel's
  # matrix doesn't grow with each new Python release. Below 3.12,
  # nanobind_add_module falls back to a version-specific build on its own.
  #
  # find_package(Python) has to run here, not just inside nanobind's own
  # CMakeLists.txt: CMake functions resolve variables from the caller's
  # scope, and nanobind_add_module is called from this scope, not
  # nanobind's FetchContent subdirectory. Without this, Python_INCLUDE_DIRS
  # is empty where nanobind_add_module reads it and the build can't find
  # Python.h.
  find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module
               OPTIONAL_COMPONENTS Development.SABIModule)

  FetchContent_Declare(
    nanobind
    GIT_REPOSITORY https://github.com/wjakob/nanobind.git
    GIT_TAG        v2.14.0
    GIT_SUBMODULES ext/robin_map
  )
  FetchContent_MakeAvailable(nanobind)

  # The name must match NB_MODULE(_native, ...) in bindings.cc.
  # Installed into pgass/, so Python imports it as pgass._native.
  nanobind_add_module(_native STABLE_ABI python/src/bindings.cc)
  target_compile_options(_native PRIVATE ${PGASS_WARNINGS})
  target_link_libraries(_native
    PRIVATE
      parse_lib
      safety_lib
      normalize_lib
      ground_lib
      solve_lib
      aspif_lib
      absl::status
      absl::statusor
      absl::strings
      absl::flat_hash_set
  )
  install(TARGETS _native LIBRARY DESTINATION pgass)
endif()
