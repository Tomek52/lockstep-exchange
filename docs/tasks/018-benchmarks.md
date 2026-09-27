# 018: Benchmarks with latency percentiles

## Goal

Produce credible, reproducible performance numbers for:

- the matching engine;
- the queues;
- the in-process engine pipeline.

Publish them in the README.

## Context

- `exchange-core/bench/` has:
  - `percentiles.hpp` (`LatencySamples`: per-operation timing exported as
    `p50_ns`/`p99_ns`/`p99.9_ns` counters);
  - `price_levels_bench.cpp` (`flat_map` vs `std::map` for price levels,
    [ADR-0009](../adr/0009-toolchain-baseline-and-feature-fallbacks.md)).
- Benchmarks are built by the `release` preset (`LOCKSTEP_BUILD_BENCHMARKS=ON`)
  and are not run in CI.
- Queue designs: [ADR-0011](../adr/0011-lock-free-queues-and-memory-ordering.md).
  Engine: [ADR-0003](../adr/0003-single-writer-sharding.md).
- Use `journal::NullJournal` to exclude I/O, and a second variant with the
  file journal (task 008), if merged, to show its cost.

## Interfaces to implement

New benchmark files (register them in `bench/CMakeLists.txt`):

- **`matching_bench.cpp`** (domain only, single thread):
  - `rest_and_cancel`: steady state around 1 000 resting orders;
  - `aggressive_fill`: a taker sweeping 1–5 levels;
  - `mixed_flow`: a replay of a fixed, seeded random command stream (same
    generator idea as the determinism test).

  Report per-command p50/p99/p99.9, plus commands/s.
- **`queue_bench.cpp`**: ping-pong round trip between two threads for
  `MutexQueue`, `SpscQueue` and `MpscQueue`. Also MPSC with 1/2/4 producers
  contending, reporting throughput and p99 enqueue latency. Pin threads with
  `pthread_setaffinity_np` when `LOCKSTEP_BENCH_PIN=1`.
- **`engine_bench.cpp`**: `Engine::submit` → completion round-trip latency
  in-process (no gRPC), with 1 and 2 shards, `NullJournal`.

Plus `scripts/run-benchmarks.sh`, which:

- builds with the release preset;
- runs all benchmarks with `--benchmark_repetitions=5`, reporting aggregates
  only;
- writes JSON to `build/bench-results/`;
- prints a Markdown table.

## Acceptance criteria

1. All benchmarks build in the release preset and run to completion.
2. `README.md` "Benchmark results" section: replace the placeholder with a
   table of p50/p99/p99.9 for each benchmark, the machine description (CPU
   model, cores, OS, compiler, whether WSL), the exact command, and the git
   commit.
3. **Findings paragraph:** a short honest paragraph, including anything that
   contradicts expectations. For example, if `std::map` beats `flat_map` at
   512 levels, say so.
4. No benchmark code in `domain/` or `app/`: all benchmark-only helpers live
   in `bench/`.
5. clang-tidy is clean on bench sources (they are compiled with
   `lockstep::compile_options`).

## Files expected to change

- `exchange-core/bench/{matching_bench.cpp,queue_bench.cpp,engine_bench.cpp,CMakeLists.txt}`
- `scripts/run-benchmarks.sh` (new)
- `README.md` (benchmark section)

## Out of scope

- gRPC end-to-end latency (that is loadgen's job, task 017).
- Continuous benchmarking in CI (noisy shared runners).

## Dependencies

- **Hard:** 002, 005, 006.
- **Soft:** 008.
