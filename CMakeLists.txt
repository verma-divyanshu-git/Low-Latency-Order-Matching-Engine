cmake_minimum_required(VERSION 3.22)

project(
  LowLatencyOrderMatchingEngine
  VERSION 0.1.0
  DESCRIPTION "A C++23 low-latency order matching engine"
  LANGUAGES CXX)

include(CTest)
include(cmake/ProjectOptions.cmake)
engine_validate_options()

# This first core slice is header-only, so consumers compile its small typed API
# without introducing a placeholder translation unit.
add_library(matching_engine_core INTERFACE)
add_library(matching_engine::core ALIAS matching_engine_core)
target_compile_features(matching_engine_core INTERFACE cxx_std_23)
target_include_directories(
  matching_engine_core
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)

if(PROJECT_IS_TOP_LEVEL)
  set(ENGINE_BUILD_TESTS_DEFAULT ON)
else()
  set(ENGINE_BUILD_TESTS_DEFAULT OFF)
endif()
option(ENGINE_BUILD_TESTS "Build matching engine tests" ${ENGINE_BUILD_TESTS_DEFAULT})

if(ENGINE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(googletest)

  add_executable(matching_engine_core_tests tests/order_arena_test.cpp)
  target_link_libraries(matching_engine_core_tests PRIVATE matching_engine::core GTest::gtest_main)
  engine_configure_target(matching_engine_core_tests)

  include(GoogleTest)
  gtest_discover_tests(matching_engine_core_tests)
endif()
