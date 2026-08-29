cmake_minimum_required(VERSION 3.22)

project(
  LowLatencyOrderMatchingEngine
  VERSION 0.1.0
  DESCRIPTION "A C++23 low-latency order matching engine"
  LANGUAGES CXX)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(cmake/ProjectOptions.cmake)
engine_validate_options()

add_library(
  matching_engine_core STATIC
  src/bbo.cpp
  src/bbo_publisher.cpp
  src/bbo_protocol.cpp
  src/command.cpp
  src/command_protocol.cpp
  src/gateway.cpp
  src/market_data_input.cpp
  src/market_data_adapter.cpp
  src/market_data_protocol.cpp
  src/order_book.cpp
  src/runtime_controls.cpp
  src/sequenced_engine.cpp)
add_library(matching_engine::core ALIAS matching_engine_core)
set_target_properties(matching_engine_core PROPERTIES EXPORT_NAME core)
target_include_directories(
  matching_engine_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
engine_configure_target(matching_engine_core)

add_library(
  matching_engine_persistence STATIC
  src/journal.cpp
  src/mbo_publisher.cpp
  src/mbp_publisher.cpp
  src/pipeline.cpp
  src/replay.cpp
  src/runtime_config.cpp
  src/runtime_operations.cpp
  src/snapshot.cpp)
add_library(matching_engine::persistence ALIAS matching_engine_persistence)
set_target_properties(matching_engine_persistence PROPERTIES EXPORT_NAME persistence)
target_include_directories(
  matching_engine_persistence
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(matching_engine_persistence PUBLIC matching_engine::core)
engine_configure_target(matching_engine_persistence)

add_executable(matching_engine_replay src/replay_cli.cpp)
target_link_libraries(matching_engine_replay PRIVATE matching_engine::persistence)
engine_configure_target(matching_engine_replay)

install(
  TARGETS matching_engine_core matching_engine_persistence matching_engine_replay
  EXPORT matching_engineTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/matching_engine DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  EXPORT matching_engineTargets
  NAMESPACE matching_engine::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/matching_engine)
configure_package_config_file(
  cmake/matching_engineConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/matching_engineConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/matching_engine)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/matching_engineConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/matching_engineConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/matching_engineConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/matching_engine)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/matching_engine)

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

  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20240116.2
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(abseil)
  if(NOT abseil_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(abseil)
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
    add_subdirectory(${abseil_SOURCE_DIR} ${abseil_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()

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

  add_library(matching_engine_throughput_gate STATIC src/throughput_gate.cpp)
  add_library(matching_engine::throughput_gate ALIAS matching_engine_throughput_gate)
  target_include_directories(
    matching_engine_throughput_gate
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_link_libraries(matching_engine_throughput_gate PRIVATE matching_engine::core)
  engine_configure_target(matching_engine_throughput_gate)

  add_library(matching_engine_benchmark_comparison STATIC src/benchmark_comparison.cpp)
  add_library(matching_engine::benchmark_comparison ALIAS matching_engine_benchmark_comparison)
  target_include_directories(
    matching_engine_benchmark_comparison
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_link_libraries(
    matching_engine_benchmark_comparison
    PRIVATE matching_engine::core absl::btree)
  engine_configure_target(matching_engine_benchmark_comparison)

  add_executable(clock_probe src/clock_probe.cpp)
  target_link_libraries(clock_probe PRIVATE matching_engine::measurement)
  engine_configure_target(clock_probe)

  add_executable(order_book_benchmark src/order_book_benchmark.cpp)
  target_link_libraries(order_book_benchmark PRIVATE matching_engine::benchmark)
  engine_configure_target(order_book_benchmark)

  add_executable(spsc_queue_benchmark src/spsc_queue_benchmark.cpp)
  target_link_libraries(spsc_queue_benchmark PRIVATE matching_engine::core)
  engine_configure_target(spsc_queue_benchmark)

  add_executable(pipeline_throughput_benchmark src/pipeline_throughput_benchmark.cpp)
  target_link_libraries(pipeline_throughput_benchmark PRIVATE matching_engine::persistence)
  engine_configure_target(pipeline_throughput_benchmark)

  add_executable(order_book_throughput_gate src/order_book_throughput_gate.cpp)
  target_link_libraries(order_book_throughput_gate PRIVATE matching_engine::throughput_gate)
  engine_configure_target(order_book_throughput_gate)

  add_executable(benchmark_comparison src/benchmark_comparison_cli.cpp)
  target_link_libraries(benchmark_comparison PRIVATE matching_engine::benchmark_comparison)
  engine_configure_target(benchmark_comparison)
  if(BUILD_TESTING)
    add_test(
      NAME order_book_throughput_gate_e2e
      COMMAND order_book_throughput_gate
              --samples
              32
              --repetitions
              3
              --min-ops-per-second
              1
              --max-relative-mad
              1)
    set_tests_properties(order_book_throughput_gate_e2e PROPERTIES TIMEOUT 10)
  endif()
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
    tests/bbo_test.cpp
    tests/bbo_publisher_test.cpp
    tests/bbo_protocol_test.cpp
    tests/command_engine_test.cpp
    tests/command_protocol_test.cpp
    tests/gateway_test.cpp
    tests/invariant_test.cpp
    tests/order_arena_test.cpp
    tests/order_book_differential_test.cpp
    tests/order_book_test.cpp
    tests/price_domain_bitmap_test.cpp
    tests/reference_order_book_test.cpp
    tests/lane_merge_test.cpp
    tests/market_data_input_test.cpp
    tests/market_data_adapter_test.cpp
    tests/market_data_protocol_test.cpp
    tests/runtime_controls_test.cpp
    tests/spsc_queue_test.cpp)
  target_compile_definitions(
    matching_engine_core_tests PRIVATE MATCHING_ENGINE_TEST_SEAMS)
  target_link_libraries(matching_engine_core_tests PRIVATE matching_engine::core GTest::gtest_main)
  engine_configure_target(matching_engine_core_tests)

  include(GoogleTest)
  gtest_discover_tests(matching_engine_core_tests)

  add_executable(spsc_queue_allocation_test tests/spsc_queue_allocation_test.cpp)
  target_link_libraries(spsc_queue_allocation_test PRIVATE matching_engine::core)
  engine_configure_target(spsc_queue_allocation_test)
  add_test(NAME spsc_queue_allocation_test COMMAND spsc_queue_allocation_test)

  add_library(
    matching_engine_persistence_test STATIC
    src/journal.cpp
    src/mbo_publisher.cpp
    src/mbp_publisher.cpp
    src/pipeline.cpp
    src/replay.cpp
    src/runtime_config.cpp
    src/runtime_operations.cpp
    src/snapshot.cpp)
  target_include_directories(
    matching_engine_persistence_test
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_compile_definitions(
    matching_engine_persistence_test PRIVATE MATCHING_ENGINE_TEST_FAILPOINTS)
  target_link_libraries(matching_engine_persistence_test PUBLIC matching_engine::core)
  engine_configure_target(matching_engine_persistence_test)

  add_executable(
    matching_engine_persistence_tests
    tests/journal_test.cpp
    tests/mbo_publisher_test.cpp
    tests/mbp_publisher_test.cpp
    tests/pipeline_test.cpp
    tests/runtime_config_test.cpp
    tests/runtime_operations_test.cpp
    tests/runtime_reliability_test.cpp
    tests/snapshot_replay_test.cpp)
  target_compile_definitions(
    matching_engine_persistence_tests PRIVATE MATCHING_ENGINE_TEST_FAILPOINTS)
  target_link_libraries(
    matching_engine_persistence_tests PRIVATE matching_engine_persistence_test GTest::gtest_main)
  engine_configure_target(matching_engine_persistence_tests)
  gtest_discover_tests(matching_engine_persistence_tests)

  add_executable(matching_engine_replay_cli_tests tests/replay_cli_test.cpp)
  target_compile_definitions(
    matching_engine_replay_cli_tests
    PRIVATE MATCHING_ENGINE_REPLAY_PATH="$<TARGET_FILE:matching_engine_replay>")
  target_link_libraries(
    matching_engine_replay_cli_tests PRIVATE matching_engine::persistence GTest::gtest_main)
  add_dependencies(matching_engine_replay_cli_tests matching_engine_replay)
  engine_configure_target(matching_engine_replay_cli_tests)
  gtest_discover_tests(matching_engine_replay_cli_tests)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME phase7_artifact_contract_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_phase7_artifacts.py)
  if(NOT ENGINE_ENABLE_ASAN AND NOT ENGINE_ENABLE_UBSAN AND NOT ENGINE_ENABLE_TSAN
     AND NOT ENGINE_ENABLE_FUZZING)
    add_test(
      NAME install_package_test
      COMMAND
        ${CMAKE_COMMAND}
        -DENGINE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DENGINE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_install.cmake)
  endif()

  if(ENGINE_BUILD_BENCHMARKS)
    add_executable(
      matching_engine_measurement_tests
      tests/benchmark_test.cpp
      tests/benchmark_comparison_test.cpp
      tests/clock_probe_cli_test.cpp
      tests/measurement_review_test.cpp
      tests/measurement_test.cpp
      tests/throughput_gate_test.cpp)
    target_link_libraries(
      matching_engine_measurement_tests
      PRIVATE matching_engine::benchmark matching_engine::benchmark_comparison
              matching_engine::throughput_gate GTest::gtest_main)
    engine_configure_target(matching_engine_measurement_tests)
    gtest_discover_tests(matching_engine_measurement_tests)
  endif()
endif()

set(CPACK_PACKAGE_NAME "matching-engine")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES "/build/;/.git/;crash-[0-9a-f]+")
include(CPack)

if(ENGINE_ENABLE_FUZZING)
  add_executable(order_book_fuzz tests/fuzz/order_book_fuzz.cpp)
  target_include_directories(order_book_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(order_book_fuzz PRIVATE matching_engine::core)
  engine_configure_fuzz_target(order_book_fuzz)

  add_executable(protocol_decoder_fuzz tests/fuzz/protocol_decoder_fuzz.cpp)
  target_link_libraries(protocol_decoder_fuzz PRIVATE matching_engine::persistence)
  engine_configure_fuzz_target(protocol_decoder_fuzz)

  if(BUILD_TESTING)
    add_test(
      NAME order_book_fuzz_smoke
      COMMAND order_book_fuzz -runs=10000 -max_len=448 -timeout=10)
    set_tests_properties(order_book_fuzz_smoke PROPERTIES LABELS fuzz TIMEOUT 120)
    add_test(
      NAME protocol_decoder_fuzz_smoke
      COMMAND protocol_decoder_fuzz -runs=10000 -max_len=65 -timeout=10)
    set_tests_properties(protocol_decoder_fuzz_smoke PROPERTIES LABELS fuzz TIMEOUT 120)
    get_filename_component(ENGINE_FUZZ_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(ENGINE_FUZZ_SYMBOLIZER "${ENGINE_FUZZ_COMPILER_DIRECTORY}/llvm-symbolizer")
    if(EXISTS "${ENGINE_FUZZ_SYMBOLIZER}")
      set_tests_properties(
        order_book_fuzz_smoke protocol_decoder_fuzz_smoke
        PROPERTIES ENVIRONMENT "ASAN_SYMBOLIZER_PATH=${ENGINE_FUZZ_SYMBOLIZER}")
    endif()
  endif()
endif()
