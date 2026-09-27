# 005: Lock-free SPSC queue

## Goal

Implement `concurrency::SpscQueue<T>` (bounded, wait-free, single producer /
single consumer) and switch the shard → publisher egress to it.

## Context

- [ADR-0011](../adr/0011-lock-free-queues-and-memory-ordering.md) specifies
  the algorithm, the memory-ordering policy (weakest correct ordering; a
  comment on every non-`seq_cst` operation) and the proof obligations. Read
  it first.
- [ADR-0003](../adr/0003-single-writer-sharding.md) explains where the queue
  sits.
- `exchange-core/concurrency/include/lockstep/concurrency/spsc_queue.hpp`
  already fixes the interface and member layout. Every member function
  currently `static_assert`s on instantiation.
- The contract is the `ConcurrentQueue` concept in `queue_concepts.hpp`:
  `try_push` must leave its argument untouched on failure.
- `MutexQueue` (`mutex_queue.hpp`) is the reference model for differential
  tests.
- The concurrency layer must not include domain or app headers
  (`ctest -L architecture`).

## Interfaces to implement

```cpp
template <typename T>
class SpscQueue {
public:
    using value_type = T;
    static constexpr bool multi_producer = false;
    static constexpr bool multi_consumer = false;
    explicit SpscQueue(std::size_t capacity);      // rounds up to a power of two, >= 2
    ~SpscQueue();                                   // destroys elements still queued
    SpscQueue(const SpscQueue&) = delete;           // and move: not movable either
    [[nodiscard]] bool try_push(T&& value);         // producer thread only
    [[nodiscard]] std::optional<T> try_pop();       // consumer thread only
    [[nodiscard]] std::size_t capacity() const noexcept;
};
```

- Elements live in raw aligned storage (`Slot`) and are constructed with
  `std::construct_at` and destroyed with `std::destroy_at`. `T` may be
  move-only and non-default-constructible.
- Indices grow monotonically (`size_t`) and are masked on access.
  Full ⇔ `tail - head == capacity`.
- Keep the cached-index optimisation from the header comment.
- Every atomic access has a comment stating what it pairs with (ADR-0011).

Then change `EgressQueue` in `exchange-core/app/include/lockstep/app/queues.hpp`
to `concurrency::SpscQueue<OutboundItem>`.

## Acceptance criteria

1. `SpscQueue<std::unique_ptr<int>>` is added to the `AllQueues` type list in
   `tests/concurrency/queue_contract_test.cpp`, and all contract tests pass.
2. New `tests/concurrency/spsc_queue_test.cpp`:
   - **Stress:** 1 producer and 1 consumer, 5 000 000 sequential integers.
     The consumer checks each value is exactly previous + 1. Must pass under
     `ctest --preset tsan`.
   - **Differential:** 100 000 random push/pop operations (fixed seed) on
     `SpscQueue` and `MutexQueue` of equal capacity, from one thread. Every
     result (`bool` / `optional`) is identical.
   - **Lifetime:** a type counting constructions and destructions shows no
     leak or double destroy when the queue is destroyed while non-empty.
   - **Capacity:** `SpscQueue<int>{5}.capacity() == 8`.
3. `EgressQueue` is switched; the whole test suite passes on debug,
   asan-ubsan and tsan.
4. clang-tidy is clean. No `seq_cst` without a justification comment.

## Files expected to change

- `exchange-core/concurrency/include/lockstep/concurrency/spsc_queue.hpp`
- `exchange-core/tests/concurrency/spsc_queue_test.cpp` (new), `tests/concurrency/CMakeLists.txt`
- `exchange-core/tests/concurrency/queue_contract_test.cpp` (type list)
- `exchange-core/app/include/lockstep/app/queues.hpp` (one line)

## Out of scope

- Benchmarks (task 018).
- Blocking or parking waits (task 007).

## Dependencies

None. Task 006 edits the same type list and `queues.hpp` (one line each).
