find_package(benchmark CONFIG REQUIRED)

# Benchmarks report latency percentiles (p50/p99/p99.9) via custom counters,
# not just means - tail latency is what matters for a matching engine.
# Run from the `release` preset: build/release/exchange-core/bench/lockstep_bench
add_executable(lockstep_bench
  price_levels_bench.cpp)
target_link_libraries(lockstep_bench PRIVATE
  lockstep::domain lockstep::concurrency lockstep::compile_options benchmark::benchmark_main)
