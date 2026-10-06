# Test layout mirrors the architecture: one executable per layer, so a layer's
# tests link only what that layer may depend on (ADR-0014).
#
# Labels drive preset filters:
#   unit          fast, deterministic, no network, no gRPC
#   determinism   replay property tests
#   architecture  include/dependency fitness functions
#   grpc          links gRPC/protobuf; excluded from the TSan preset (ADR-0007)
#   fuzz          short libFuzzer smoke runs (asan-ubsan preset only)

set(LOCKSTEP_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LOCKSTEP_CORE_DIR ${PROJECT_SOURCE_DIR}/exchange-core)

# lockstep_add_test(<name> LABELS <label>... SOURCES <src>... LINK <lib>...)
function(lockstep_add_test name)
  cmake_parse_arguments(ARG "" "" "LABELS;SOURCES;LINK" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_include_directories(${name} PRIVATE ${LOCKSTEP_TESTS_DIR})
  target_link_libraries(${name} PRIVATE
    ${ARG_LINK} lockstep::compile_options GTest::gtest GTest::gmock GTest::gtest_main)
  # Exactly one label per executable: gtest_discover_tests (CMake 3.28)
  # flattens list-valued properties, so "unit;grpc" would silently become
  # "unit" and the TSan preset's label filter would stop excluding gRPC tests.
  list(LENGTH ARG_LABELS _count)
  if(NOT _count EQUAL 1)
    message(FATAL_ERROR "lockstep_add_test(${name}): exactly one label is supported")
  endif()
  gtest_discover_tests(${name}
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS ${ARG_LABELS})
endfunction()

# lockstep_add_include_rule(<layer> <root> ALLOWED_PROJECT <prefix>... [ALLOWED_STD <hdr>...])
function(lockstep_add_include_rule layer root)
  cmake_parse_arguments(ARG "" "" "ALLOWED_PROJECT;ALLOWED_STD" ${ARGN})
  string(REPLACE ";" "\\;" _project "${ARG_ALLOWED_PROJECT}")
  set(_args -DLAYER=${layer} -DROOT=${root} "-DALLOWED_PROJECT=${_project}")
  if(ARG_ALLOWED_STD)
    string(REPLACE ";" "\\;" _std "${ARG_ALLOWED_STD}")
    list(APPEND _args "-DALLOWED_STD=${_std}")
  endif()
  add_test(NAME architecture.${layer}_includes
    COMMAND ${CMAKE_COMMAND} ${_args} -P ${LOCKSTEP_TESTS_DIR}/architecture/check_includes.cmake)
  set_tests_properties(architecture.${layer}_includes PROPERTIES LABELS architecture)
endfunction()

# One directory per layer, so work on different layers never edits the same
# CMakeLists (docs/tasks/README.md explains how tasks are parallelised).
add_subdirectory(architecture)
add_subdirectory(domain)
add_subdirectory(concurrency)
add_subdirectory(app)
add_subdirectory(journal)
add_subdirectory(determinism)
add_subdirectory(codec)
add_subdirectory(grpc)
add_subdirectory(config)
add_subdirectory(main)
