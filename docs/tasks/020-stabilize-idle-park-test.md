# 020: Stabilise the idle-parking test under CPU load

## Goal

Make `EnginePipelineTest.IdleShardParksRatherThanPolling` pass on a loaded
machine, without weakening what it proves: an idle shard parks instead of
polling. Then check that no other test in the runtime suites has the same
flaw.

## Context

- The test comes from [task 007](007-runtime-idle-and-stats.md) (acceptance
  criterion 2) and lives in `exchange-core/tests/app/engine_pipeline_test.cpp`.
- It starts an idle engine, sleeps a fixed 20 ms, reads `Engine::shard_stats()`
  and asserts `parks >= 1` for every shard (`EXPECT_GE(after_settle[i].parks, 1U)`,
  line 165 at the time of writing). It then sleeps 200 ms and asserts
  `parks <= 5`.
- The 20 ms assumes the shard thread has run through the spin (256 iterations)
  and yield (64 iterations) phases of `ParkingIdle` and reached the park
  phase. `ParkingIdle` publishes `parks_stat_` as soon as it enters
  `Doorbell::wait()`
  (`exchange-core/concurrency/include/lockstep/concurrency/idle_strategy.hpp`),
  so the counter is correct as soon as the thread has been scheduled for long
  enough. Whether it has been is a property of the machine, not of the code.
- **Observed failure** (found while reviewing task 014): the test fails
  deterministically on the `asan-ubsan` preset when the CPU is saturated
  (2 of 2 runs with 8 busy loops on an 8-core WSL2 host) and passes on an
  idle machine. The `tsan` preset passed under the same load. The failure is
  in the *positive* assertion (`parks >= 1`): the shard thread simply had not
  been scheduled to reach the park phase within 20 ms. It is a test defect,
  not a runtime defect: a CI runner that is busier than a developer machine
  will hit it too.
- The same file and `tests/concurrency/idle_strategy_test.cpp` contain other
  fixed sleeps. Only a sleep followed by a *positive* assertion on thread
  state can fail falsely under load. Sleeps that "let it park" before a
  scenario that must work whether or not the thread has parked, and checks
  that something did **not** happen, can only lose coverage, not fail.
- [ADR-0011](../adr/0011-lock-free-queues-and-memory-ordering.md): memory
  orderings need comments. Not changed by this task.

## Interfaces to implement

No production interface changes. A small test helper is allowed in
`exchange-core/tests/app/test_support.hpp` (next to `reply_timeout`, namespace
`test`) if more than one test needs it, or local to the test file otherwise:

```cpp
/// Polls `pred` until it returns true or `timeout` elapses. Returns the last
/// value of `pred`. Never sleeps for the full timeout when the condition
/// holds early.
template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate pred,
                              std::chrono::milliseconds timeout = std::chrono::seconds{10});
```

The fix for the test: replace the fixed 20 ms with `wait_until` on
"every shard reports `parks >= 1`", then take `after_settle`, keep the 200 ms
idle window, and keep the `parks <= 5` ceiling as it is.

## Acceptance criteria

1. **Reproduction recorded.** The PR description gives the exact commands
   that reproduce the failure before the fix (the `asan-ubsan` preset with
   the CPU saturated, for example one `sh -c 'while :; do :; done'` per core
   while running `ctest --preset asan-ubsan -R IdleShardParksRatherThanPolling`)
   and the result before and after.
2. **Stable under load.** With the CPU saturated as in criterion 1,
   `IdleShardParksRatherThanPolling` passes in 20 consecutive runs on
   `asan-ubsan`, and in 20 on `tsan`.
3. **Still catches both bugs.** The test still fails if the strategy never
   parks (`parks` stays 0, so the wait times out) and if it polls instead of
   parking (`parks` grows past the ceiling in the idle window). Show each by
   a temporary mutation of `ParkingIdle` in the PR description (not
   committed): one that never reaches `wait()`, and one that returns from
   `wait()` immediately.
4. **No fixed sleep before a positive assertion.** In `tests/app` and
   `tests/concurrency` no test asserts positive thread state (a counter
   `>= n`, a flag set, a thread parked) right after a fixed sleep. The PR
   description lists every `sleep_for` in those two directories with a
   one-line verdict (fixed, or why it cannot fail falsely). Any that can are
   fixed with the same `wait_until` approach.
5. **Nothing else changes.** No file under `exchange-core/app/` or
   `exchange-core/concurrency/` is modified. All five presets, clang-tidy,
   format, docs-check and `scripts/e2e-smoke.sh` pass on an idle machine.

## Files expected to change

- `exchange-core/tests/app/engine_pipeline_test.cpp`
- `exchange-core/tests/app/test_support.hpp` (only if a shared helper is added)
- `exchange-core/tests/concurrency/idle_strategy_test.cpp`,
  `exchange-core/tests/app/publisher_test.cpp` (only if the audit in
  criterion 4 finds a flawed sleep)

## Out of scope

- Changing `ParkingIdle`, `Doorbell` or `ShardRuntime`.
- Raising the 20 ms to a larger constant (a bigger sleep only moves the
  failure to a slower machine and slows the suite).
- Making tests pass under saturated CPU in general; only the false failures
  caused by fixed sleeps.
- Excluding the test from sanitizer presets.

## Dependencies

None hard. Builds on [007](007-runtime-idle-and-stats.md), which is done.
