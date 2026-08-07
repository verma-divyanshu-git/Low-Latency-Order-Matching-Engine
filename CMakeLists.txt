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
    tests/order_arena_test.cpp
    tests/order_book_differential_test.cpp
    tests/order_book_test.cpp
    tests/price_domain_bitmap_test.cpp
    tests/reference_order_book_test.cpp)
  target_link_libraries(matching_engine_core_tests PRIVATE matching_engine::core GTest::gtest_main)
  engine_configure_target(matching_engine_core_tests)

  include(GoogleTest)
  gtest_discover_tests(matching_engine_core_tests)
endif()
