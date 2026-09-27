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
