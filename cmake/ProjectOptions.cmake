include_guard(GLOBAL)

include(CheckCXXSourceCompiles)

option(ENGINE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENGINE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENGINE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENGINE_ENABLE_FUZZING "Enable libFuzzer instrumentation" OFF)

function(engine_validate_options)
  if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "/std:c++latest")
  else()
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")
  endif()
  check_cxx_source_compiles(
    [=[
      #include <expected>
      #include <version>

      #if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
      #error "C++23 std::expected is unavailable"
      #endif

      int main() {
        const std::expected<int, int> result{1};
        return *result - 1;
      }
    ]=]
    ENGINE_HAS_CXX23_EXPECTED)
  if(NOT ENGINE_HAS_CXX23_EXPECTED)
    message(
      FATAL_ERROR
        "The selected C++23 standard library does not provide std::expected; use GCC 12+, Clang with a compatible modern standard library, or Apple Clang 15+"
    )
  endif()

  set(sanitizer_count 0)
  foreach(option_name ENGINE_ENABLE_ASAN ENGINE_ENABLE_UBSAN ENGINE_ENABLE_TSAN)
    if(${option_name})
      math(EXPR sanitizer_count "${sanitizer_count} + 1")
    endif()
  endforeach()
  if(sanitizer_count GREATER 1)
    message(FATAL_ERROR "Sanitizer presets are intentionally mutually exclusive")
  endif()
  if(ENGINE_ENABLE_FUZZING AND sanitizer_count GREATER 0)
    message(FATAL_ERROR "Fuzzing already supplies its supported ASan combination")
  endif()
  if((sanitizer_count GREATER 0 OR ENGINE_ENABLE_FUZZING)
     AND NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(
      FATAL_ERROR
        "Sanitizer presets do not support compiler '${CMAKE_CXX_COMPILER_ID}'; use GCC or Clang"
    )
  endif()
  if(ENGINE_ENABLE_FUZZING AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The fuzz preset requires Clang with libFuzzer")
  endif()
  if(ENGINE_ENABLE_FUZZING)
    set(CMAKE_REQUIRED_FLAGS
        "-fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer")
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address)
    check_cxx_source_compiles(
      "int main() { return 0; }" ENGINE_HAS_WORKING_FUZZER_NO_LINK)
    if(NOT ENGINE_HAS_WORKING_FUZZER_NO_LINK)
      message(
        FATAL_ERROR
          "The fuzz preset requires Clang with working fuzzer-no-link and AddressSanitizer support; '${CMAKE_CXX_COMPILER}' could not compile and link an instrumented executable"
      )
    endif()

    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address -fsanitize=fuzzer)
    check_cxx_source_compiles(
      [=[
        #include <cstddef>
        #include <cstdint>

        extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) {
          return 0;
        }
      ]=]
      ENGINE_HAS_WORKING_LIBFUZZER)
    if(NOT ENGINE_HAS_WORKING_LIBFUZZER)
      message(
        FATAL_ERROR
          "The fuzz preset requires Clang with working libFuzzer and AddressSanitizer runtimes; '${CMAKE_CXX_COMPILER}' could not compile and link a fuzz harness"
      )
    endif()
  endif()
endfunction()

function(engine_configure_target target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "engine_configure_target requires an existing target")
  endif()

  target_compile_features("${target}" PUBLIC cxx_std_23)
  set_target_properties("${target}" PROPERTIES CXX_EXTENSIONS OFF)

  if(MSVC)
    target_compile_options("${target}" PRIVATE /W4 /permissive-)
  else()
    target_compile_options(
      "${target}"
      PRIVATE -Wall
              -Wextra
              -Wpedantic
              -Wconversion
              -Wsign-conversion
              -Wshadow
              -Wformat=2
              -Wundef
              -Werror=return-type)
  endif()

  if(ENGINE_ENABLE_ASAN)
    target_compile_options("${target}" PRIVATE -fsanitize=address
                                                -fno-omit-frame-pointer)
    target_link_options("${target}" PRIVATE -fsanitize=address)
  elseif(ENGINE_ENABLE_UBSAN)
    target_compile_options("${target}" PRIVATE -fsanitize=undefined
                                                -fno-omit-frame-pointer)
    target_link_options("${target}" PRIVATE -fsanitize=undefined)
  elseif(ENGINE_ENABLE_TSAN)
    target_compile_options("${target}" PRIVATE -fsanitize=thread
                                                -fno-omit-frame-pointer)
    target_link_options("${target}" PRIVATE -fsanitize=thread)
  endif()

  if(ENGINE_ENABLE_FUZZING)
    target_compile_options("${target}" PRIVATE -fsanitize=fuzzer-no-link,address
                                                -fno-omit-frame-pointer)
    target_link_options("${target}" PRIVATE -fsanitize=address)
  endif()
endfunction()

function(engine_configure_fuzz_target target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "engine_configure_fuzz_target requires an existing target")
  endif()
  if(NOT ENGINE_ENABLE_FUZZING)
    message(FATAL_ERROR "engine_configure_fuzz_target requires the fuzz preset")
  endif()
  get_target_property(target_type "${target}" TYPE)
  if(NOT target_type STREQUAL "EXECUTABLE")
    message(FATAL_ERROR "A fuzz harness target must be an executable")
  endif()

  engine_configure_target("${target}")
  target_link_options("${target}" PRIVATE -fsanitize=fuzzer)
endfunction()
