# Project-wide compile options, exposed as an INTERFACE target so that every
# lockstep target opts in explicitly (and generated protobuf code does not).

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++20 modules in this project (ADR-0010); skip CMake's dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(lockstep_compile_options INTERFACE)
add_library(lockstep::compile_options ALIAS lockstep_compile_options)

target_compile_features(lockstep_compile_options INTERFACE cxx_std_23)

target_compile_options(lockstep_compile_options INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Woverloaded-virtual
  -Wnull-dereference
  -Wimplicit-fallthrough
  -Wcast-align
  -Wdouble-promotion
  $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast>
  $<$<CXX_COMPILER_ID:Clang>:-Wthread-safety -Wunreachable-code-aggressive>
  $<$<BOOL:${LOCKSTEP_WERROR}>:-Werror>)

# lockstep_allow_std_generator(<source>...)
#
# GCC 14 at -O2/-O3 reports -Wnull-dereference inside libstdc++'s <generator>
# and <coroutine> (coroutine_handle::promise() after inlining) wherever a
# std::generator is created or iterated. The reports point at standard-library
# lines, not ours, and -Werror turns them into release-build failures. Call this
# in the directory of the target, for exactly the sources that create or iterate
# a generator, so the warning stays on everywhere else. Source-level options
# follow the target's, so -Wno-null-dereference overrides the one above.
function(lockstep_allow_std_generator)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS -Wno-null-dereference)
  endif()
endfunction()
