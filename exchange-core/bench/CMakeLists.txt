find_package(benchmark CONFIG REQUIRED)

# Benchmarks report latency percentiles (p50/p99/p99.9) via custom counters,
# not just means - tail latency is what matters for a matching engine.
# Built by every preset that leaves LOCKSTEP_BUILD_BENCHMARKS on; numbers are
# only meaningful from the `release` preset. scripts/run-benchmarks.sh builds
# and runs all of them; they are not run in CI (shared runners are too noisy).
#
# One executable per area so a run can be filtered, pinned and recorded
# separately. All helpers (percentiles, pinning) live here, not in domain/ or
# app/ (task 018 criterion 4).
function(lockstep_add_bench name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE
    ${ARG_LINK} lockstep::compile_options benchmark::benchmark_main)
endfunction()

lockstep_add_bench(lockstep_bench
  SOURCES price_levels_bench.cpp
  LINK lockstep::domain lockstep::concurrency)
lockstep_add_bench(lockstep_bench_matching
  SOURCES matching_bench.cpp
  LINK lockstep::domain)
lockstep_add_bench(lockstep_bench_queue
  SOURCES queue_bench.cpp
  LINK lockstep::concurrency)
lockstep_add_bench(lockstep_bench_engine
  SOURCES engine_bench.cpp
  LINK lockstep::app lockstep::journal)
