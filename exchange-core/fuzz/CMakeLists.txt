# libFuzzer targets (Clang only). Built by the asan-ubsan preset, so every
# crash is also checked for memory errors and undefined behaviour.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(WARNING "LOCKSTEP_BUILD_FUZZERS requires Clang; skipping fuzz targets")
  return()
endif()

add_executable(order_entry_decode_fuzzer order_entry_decode_fuzzer.cpp)
target_link_libraries(order_entry_decode_fuzzer PRIVATE lockstep::codec lockstep::compile_options)
target_compile_options(order_entry_decode_fuzzer PRIVATE -fsanitize=fuzzer)
target_link_options(order_entry_decode_fuzzer PRIVATE -fsanitize=fuzzer)

# Smoke-run in CTest: a few thousand iterations from an empty corpus. CI runs
# a longer time-boxed session (see .github/workflows/ci.yml).
add_test(NAME fuzz.order_entry_decode_smoke
  COMMAND order_entry_decode_fuzzer -runs=20000 -seed=1)
set_tests_properties(fuzz.order_entry_decode_smoke PROPERTIES LABELS fuzz)

add_executable(journal_decode_fuzzer journal_decode_fuzzer.cpp)
target_link_libraries(journal_decode_fuzzer PRIVATE lockstep::journal lockstep::compile_options)
target_compile_options(journal_decode_fuzzer PRIVATE -fsanitize=fuzzer)
target_link_options(journal_decode_fuzzer PRIVATE -fsanitize=fuzzer)

add_test(NAME fuzz.journal_decode_smoke
  COMMAND journal_decode_fuzzer -runs=20000 -seed=1)
set_tests_properties(fuzz.journal_decode_smoke PROPERTIES LABELS fuzz)
