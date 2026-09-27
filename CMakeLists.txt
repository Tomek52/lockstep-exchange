cmake_minimum_required(VERSION 3.28)

project(lockstep
  VERSION 0.1.0
  DESCRIPTION "Deterministic, lock-free exchange engine (C++23) with a Rust risk sentinel"
  LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use a preset: cmake --preset debug")
endif()

option(LOCKSTEP_BUILD_TESTS      "Build unit, determinism and architecture tests" ON)
option(LOCKSTEP_BUILD_BENCHMARKS "Build Google Benchmark targets" ON)
option(LOCKSTEP_BUILD_FUZZERS    "Build libFuzzer targets (Clang only)" OFF)
option(LOCKSTEP_WERROR           "Treat compiler warnings as errors" OFF)
set(LOCKSTEP_SANITIZER "" CACHE STRING "Sanitizer set: '', 'address;undefined' or 'thread'")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CompilerOptions)   # lockstep_compile_options INTERFACE target
include(Sanitizers)        # applies LOCKSTEP_SANITIZER to every target in the build
include(FeatureProbe)      # verifies the C++23 baseline, detects fallbacks (ADR-0009)
include(ArchitectureRules) # link allow-lists for layers (ADR-0002)
include(ProtoCodegen)      # protoc + grpc_cpp_plugin

if(LOCKSTEP_BUILD_TESTS)
  enable_testing()
  find_package(GTest 1.14 CONFIG REQUIRED)
  include(GoogleTest)
endif()

add_subdirectory(exchange-core)

lockstep_print_feature_summary()
