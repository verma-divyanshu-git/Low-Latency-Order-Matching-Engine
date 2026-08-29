cmake_minimum_required(VERSION 3.22)
project(MatchingEnginePackageConsumer LANGUAGES CXX)

find_package(matching_engine CONFIG REQUIRED)

if(NOT TARGET matching_engine::core OR NOT TARGET matching_engine::persistence)
  message(FATAL_ERROR "matching engine exported targets are incomplete")
endif()

add_executable(package_consumer main.cpp)
target_compile_features(package_consumer PRIVATE cxx_std_23)
target_link_libraries(package_consumer PRIVATE matching_engine::persistence)
