cmake_minimum_required(VERSION 3.22)

project(
  LowLatencyOrderMatchingEngine
  VERSION 0.1.0
  DESCRIPTION "A C++23 low-latency order matching engine"
  LANGUAGES CXX)

include(CTest)
include(cmake/ProjectOptions.cmake)
engine_validate_options()

add_library(matching_engine_core STATIC src/order_book.cpp)
add_library(matching_engine::core ALIAS matching_engine_core)
target_include_directories(
  matching_engine_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
engine_configure_target(matching_engine_core)

if(PROJECT_IS_TOP_LEVEL)
  set(ENGINE_BUILD_TESTS_DEFAULT ON)
else()
  set(ENGINE_BUILD_TESTS_DEFAULT OFF)
endif()
option(ENGINE_BUILD_TESTS "Build matching engine tests" ${ENGINE_BUILD_TESTS_DEFAULT})
option(ENGINE_BUILD_BENCHMARKS "Build measurement tools and benchmarks" OFF)

if(ENGINE_BUILD_BENCHMARKS)
  enable_language(C)
  include(FetchContent)
  set(HDR_LOG_REQUIRED DISABLED CACHE STRING "" FORCE)
  set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_INSTALL_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_INSTALL_STATIC OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    hdr_histogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG 18c7a324383dded1451d15621cd018b0048057d0
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(hdr_histogram)

  add_library(matching_engine_measurement STATIC src/clock_probe_cli.cpp src/measurement.cpp)
  add_library(matching_engine::measurement ALIAS matching_engine_measurement)
  target_include_directories(
    matching_engine_measurement
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  engine_configure_target(matching_engine_measurement)

  add_library(matching_engine_benchmark STATIC src/benchmark.cpp)
  add_library(matching_engine::benchmark ALIAS matching_engine_benchmark)
  target_include_directories(
    matching_engine_benchmark
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_link_libraries(
    matching_engine_benchmark
    PUBLIC matching_engine::measurement
    PRIVATE matching_engine::core hdr_histogram::hdr_histogram_static)
  engine_configure_target(matching_engine_benchmark)

  add_executable(clock_probe src/clock_probe.cpp)
  target_link_libraries(clock_probe PRIVATE matching_engine::measurement)
  engine_configure_target(clock_probe)

  add_executable(order_book_benchmark src/order_book_benchmark.cpp)
  target_link_libraries(order_book_benchmark PRIVATE matching_engine::benchmark)
  engine_configure_target(order_book_benchmark)
endif()

if(BUILD_TESTING AND ENGINE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(googletest)

  add_executable(
    matching_engine_core_tests
    tests/invariant_test.cpp
    tests/order_arena_test.cpp
    tests/order_book_differential_test.cpp
    tests/order_book_test.cpp
    tests/price_domain_bitmap_test.cpp
    tests/reference_order_book_test.cpp)
  target_link_libraries(matching_engine_core_tests PRIVATE matching_engine::core GTest::gtest_main)
  engine_configure_target(matching_engine_core_tests)

  include(GoogleTest)
  gtest_discover_tests(matching_engine_core_tests)

  if(ENGINE_BUILD_BENCHMARKS)
    add_executable(
      matching_engine_measurement_tests
      tests/benchmark_test.cpp
      tests/clock_probe_cli_test.cpp
      tests/measurement_review_test.cpp
      tests/measurement_test.cpp)
    target_link_libraries(
      matching_engine_measurement_tests
      PRIVATE matching_engine::benchmark GTest::gtest_main)
    engine_configure_target(matching_engine_measurement_tests)
    gtest_discover_tests(matching_engine_measurement_tests)
  endif()
endif()

if(ENGINE_ENABLE_FUZZING)
  add_executable(order_book_fuzz tests/fuzz/order_book_fuzz.cpp)
  target_include_directories(order_book_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(order_book_fuzz PRIVATE matching_engine::core)
  engine_configure_fuzz_target(order_book_fuzz)

  if(BUILD_TESTING)
    add_test(
      NAME order_book_fuzz_smoke
      COMMAND order_book_fuzz -runs=10000 -max_len=448 -timeout=10)
    set_tests_properties(order_book_fuzz_smoke PROPERTIES LABELS fuzz TIMEOUT 120)
    get_filename_component(ENGINE_FUZZ_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(ENGINE_FUZZ_SYMBOLIZER "${ENGINE_FUZZ_COMPILER_DIRECTORY}/llvm-symbolizer")
    if(EXISTS "${ENGINE_FUZZ_SYMBOLIZER}")
      set_tests_properties(
        order_book_fuzz_smoke
        PROPERTIES ENVIRONMENT "ASAN_SYMBOLIZER_PATH=${ENGINE_FUZZ_SYMBOLIZER}")
    endif()
  endif()
endif()
