include_guard(GLOBAL)

option(ENGINE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENGINE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENGINE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENGINE_ENABLE_FUZZING "Enable libFuzzer instrumentation" OFF)

function(engine_validate_options)
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
  if((ENGINE_ENABLE_ASAN OR ENGINE_ENABLE_UBSAN OR ENGINE_ENABLE_TSAN) AND MSVC)
    message(FATAL_ERROR "Sanitizer presets require GCC or Clang")
  endif()
  if(ENGINE_ENABLE_FUZZING AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The fuzz preset requires Clang and libFuzzer")
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
    target_compile_options("${target}" PRIVATE -fsanitize=fuzzer,address
                                                -fno-omit-frame-pointer)
    target_link_options("${target}" PRIVATE -fsanitize=fuzzer,address)
  endif()
endfunction()
