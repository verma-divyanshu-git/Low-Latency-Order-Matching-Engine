if(NOT DEFINED ENGINE_BINARY_DIR OR NOT DEFINED ENGINE_SOURCE_DIR)
  message(FATAL_ERROR "ENGINE_BINARY_DIR and ENGINE_SOURCE_DIR are required")
endif()

set(test_root "${ENGINE_BINARY_DIR}/install-consumer-test")
file(REMOVE_RECURSE "${test_root}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${ENGINE_BINARY_DIR}" --prefix "${test_root}/prefix"
  RESULT_VARIABLE install_result
  OUTPUT_QUIET
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed: ${install_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${ENGINE_SOURCE_DIR}/tests/package_consumer"
          -B "${test_root}/build"
          "-DCMAKE_PREFIX_PATH=${test_root}/prefix"
  RESULT_VARIABLE configure_result
  OUTPUT_QUIET
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "consumer configure failed: ${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${test_root}/build"
  RESULT_VARIABLE build_result
  OUTPUT_QUIET
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed: ${build_error}")
endif()
