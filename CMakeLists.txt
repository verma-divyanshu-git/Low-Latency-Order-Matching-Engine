cmake_minimum_required(VERSION 3.22)

project(
  LowLatencyOrderMatchingEngine
  VERSION 0.1.0
  DESCRIPTION "A C++23 low-latency order matching engine"
  LANGUAGES CXX)

include(CTest)
include(cmake/ProjectOptions.cmake)
engine_validate_options()

# Production, test, and benchmark targets will be added with their first real
# implementations. Keeping Phase 0 target-free avoids placeholder code and
# unnecessary dependencies.
