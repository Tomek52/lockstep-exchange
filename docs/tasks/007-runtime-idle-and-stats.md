# 007: Parking idle strategy, wake-ups and runtime statistics

## Goal

Let idle shard and publisher threads park (no CPU, no fixed sleep latency)
and wake promptly when work arrives or stop is requested. Expose basic
runtime counters.

## Context

- The skeleton uses `concurrency::BackoffIdle`
  (`exchange-core/concurrency/include/lockstep/concurrency/idle_strategy.hpp`):
  spin, then yield, then `sleep_for(50µs)`. The sleep adds up to 50 µs of
  latency to the first command after an idle period. That is visible as the
  ~1 ms p50 in `scripts/e2e-smoke.sh`, together with the gRPC overhead.
- Idle strategies are chosen in `exchange-core/app/include/lockstep/app/queues.hpp`
  (`RuntimeIdle`) and must satisfy the `IdleStrategy` concept.
- Run loops: `ShardRuntime::run` and `Publisher::run` in `exchange-core/app/src/`.
- [ADR-0003](../adr/0003-single-writer-sharding.md): shutdown uses
  `std::stop_token` and must remain lossless.
- [ADR-0011](../adr/0011-lock-free-queues-and-memory-ordering.md): memory
  orderings need comments.

## Interfaces to implement

```cpp
// concurrency/idle_strategy.hpp
/// A counter consumers park on and producers bump. Producers call ring()
/// after a successful push; consumers call wait(seen, stop) when idle.
class Doorbell {
public:
    [[nodiscard]] std::uint64_t value() const noexcept;
    void ring() noexcept;                                      // fetch_add + notify_one
    void wait(std::uint64_t seen, const std::stop_token& stop); // returns on change or stop
};

/// Spin -> yield -> park on a Doorbell.
class ParkingIdle { /* satisfies IdleStrategy; constructed with a Doorbell& */ };
```

- `ShardRuntime` owns a `Doorbell`. `Engine::submit` and `Engine::broadcast`
  ring the target shard's doorbell after a successful push. The shard ends
  its egress release by ringing the publisher's doorbell.
- Waking on stop: use `std::stop_callback` to ring the doorbell when stop is
  requested, so parked threads observe it.
- **Avoid lost wake-ups.** The consumer reads `value()` *before* its final
  empty check and passes that value to `wait()`. Document the argument in a
  comment.
- Statistics, read only after stop or through relaxed atomics (comment why):

  ```cpp
  struct ShardStats { std::uint64_t commands; std::uint64_t batches;
                      std::uint64_t max_batch; std::uint64_t parks; };
  ShardStats ShardRuntime::stats() const;   // Engine exposes all shards
  ```

## Acceptance criteria

1. **Doorbell tests** (`tests/concurrency/idle_strategy_test.cpp`):
   - `wait` returns promptly after `ring` from another thread;
   - `wait` returns when stop is requested;
   - no lost wake-up in 100 000 ping-pong rounds between two threads.
   All three pass under tsan.
2. **Idle is cheap:** after 200 ms with no traffic, a shard's `parks` counter
   has increased by at most a small bound (for example ≤ 5). This shows the
   thread is parked, not polling.
3. **Prompt shutdown:** `Engine::stop()` on an idle engine returns within
   100 ms (test with `steady_clock`, generous bound).
4. **Nothing breaks:** all existing tests in `tests/app` and
   `tests/determinism` pass on debug, asan-ubsan and tsan.
5. **Documented latency effect:** `scripts/e2e-smoke.sh` still passes.
   Record the before/after loadgen p50 in the PR description.

## Files expected to change

- `exchange-core/concurrency/include/lockstep/concurrency/idle_strategy.hpp`
- `exchange-core/app/include/lockstep/app/{queues.hpp,shard_runtime.hpp,publisher.hpp,engine.hpp}`
- `exchange-core/app/src/{shard_runtime.cpp,publisher.cpp,engine.cpp}`
- `exchange-core/tests/concurrency/idle_strategy_test.cpp`, `exchange-core/tests/app/engine_pipeline_test.cpp`

## Out of scope

- The lock-free queues themselves (005/006). This task works with any
  `ConcurrentQueue`.
- Metrics export over the network.

## Dependencies

None. **Soft:** land after 005/006, so latency is measured with the final
queues.
