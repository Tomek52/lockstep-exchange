# 011: Publisher subscriptions with a slow-consumer policy

## Goal

Let consumers (market data streams, the risk client) subscribe and
unsubscribe **at runtime**, each with its own bounded buffer, so that a slow
consumer is disconnected instead of stalling the publisher, and through it
every shard.

## Context

- [ADR-0003](../adr/0003-single-writer-sharding.md): the publisher is the
  single consumer of every shard's egress and must never block.
- [ADR-0006](../adr/0006-grpc-service-and-stream-design.md): slow consumers
  are dropped with `RESOURCE_EXHAUSTED`, and events carry
  `(shard, sequence)` for gap detection.
- Today, `Publisher`
  (`exchange-core/app/include/lockstep/app/publisher.hpp`, `src/publisher.cpp`)
  calls `EventSubscriber::on_events` synchronously, and subscribers are
  registered only before start (`Engine::add_subscriber`).
- The app layer may not include gRPC or protobuf (`ctest -L architecture`).
  Subscriptions must be transport-agnostic.

## Interfaces to implement

```cpp
// app/include/lockstep/app/subscription.hpp
struct SubscriptionFilter {
    std::vector<domain::InstrumentId> instruments;   // empty = all
    bool trades{true};
    bool book_updates{true};
    bool private_events{false};  // OrderAccepted/Cancelled/Modified, RiskCommandApplied (risk client)
};
```

`InstrumentStatusChanged` is gated by none of the three booleans above: a
halt/resume is always delivered, subject only to the instrument filter -
`market_data.proto` carries no status flag of its own, and an order-entry
client needs to know about a halt regardless of what else it asked for.
`RiskCommandApplied` carries no instrument (a per-shard broadcast ack, not
scoped to one book), so the instrument filter never excludes it; it is still
gated by `private_events`.

```cpp

/// Consumer end of a subscription. Thread-safe to use from ONE consumer thread
/// (e.g. a gRPC reactor) while the publisher produces.
class Subscription {
public:
    /// Pops up to out.size() events; returns how many were written.
    std::size_t poll(std::span<PublishedEvent> out);
    /// True once the buffer overflowed; no further events are delivered.
    [[nodiscard]] bool overflowed() const noexcept;
    /// Called by the consumer to stop delivery (idempotent).
    void cancel() noexcept;
    /// Optional wake-up hook the publisher calls (on its thread) after
    /// delivering; must not block. Set before first poll.
    void on_ready(std::move_only_function<void() noexcept> callback);
};

// Engine / Publisher
std::shared_ptr<Subscription> Engine::subscribe(SubscriptionFilter filter, std::size_t capacity);
```

- **Registration is thread-safe** from any thread. Hand requests to the
  publisher thread through a small MPSC control queue, so that the
  publisher's subscriber list is still touched only by the publisher.
- **Delivery** goes into a per-subscription SPSC ring (publisher → consumer).
  Use `SpscQueue` if task 005 has landed, else `MutexQueue`.
- **Overflow:** set `overflowed` (atomic, with ordering comment), stop
  delivering, drop the subscription on the next publisher iteration, and
  invoke `on_ready` once so the consumer notices.
- **Compatibility:** keep `EventSubscriber` and `add_subscriber` working.
  They are the synchronous path, used today by the risk client.

## Acceptance criteria

Tests in `tests/app/publisher_test.cpp`:

1. A subscription filtered to instrument 2 receives only instrument-2 events,
   in shard sequence order.
2. **Slow consumer:** with capacity 8 and a consumer that never polls, the
   publisher keeps delivering to a second, fast subscription. The engine
   processes 10 000 commands without blocking (bounded time).
   `overflowed()` becomes true and `on_ready` fires.
3. Subscribing and cancelling from 4 threads concurrently while traffic flows
   passes under tsan with no leaks under asan.
4. A subscription created mid-stream receives only events published after
   it was registered, with no partial command. The events of one command
   are delivered atomically with respect to registration.
5. Existing `tests/app` and `tests/determinism` pass.
6. Presets debug, asan-ubsan and tsan pass; clang-tidy is clean.

## Files expected to change

- `exchange-core/app/include/lockstep/app/{subscription.hpp,publisher.hpp,engine.hpp}`
- `exchange-core/app/src/{subscription.cpp,publisher.cpp,engine.cpp}`, `app/CMakeLists.txt`
- `exchange-core/tests/app/publisher_test.cpp` (new), `tests/app/CMakeLists.txt`

## Out of scope

- The gRPC `MarketDataService` that consumes subscriptions (task 013).
- Snapshot-on-subscribe (a new subscriber receiving the current book): note
  it as a follow-up.

## Dependencies

None. **Soft:** 005 (use `SpscQueue` for per-subscription rings).
