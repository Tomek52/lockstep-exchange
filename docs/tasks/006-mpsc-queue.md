# 006: Lock-free bounded MPSC queue

## Goal

Implement `concurrency::MpscQueue<T>` (Dmitry Vyukov's bounded queue,
specialised for a single consumer) and switch the shard ingress to it.

## Context

- [ADR-0011](../adr/0011-lock-free-queues-and-memory-ordering.md) specifies
  the algorithm, the orderings and the proof obligations. Read it first.
- `exchange-core/concurrency/include/lockstep/concurrency/mpsc_queue.hpp`
  already fixes the interface and layout: per-slot atomic `sequence`,
  contended `enqueue_pos_`, and consumer-private `dequeue_pos_`, each on its
  own cache line. The member functions `static_assert` on instantiation.
- Contract: the `ConcurrentQueue` / `MultiProducerQueue` concepts in
  `queue_concepts.hpp`. `try_push` leaves its argument untouched on failure.
- Producers are gRPC callback threads and the risk client
  ([ADR-0003](../adr/0003-single-writer-sharding.md)).
- Reference model for differential tests: `MutexQueue`.

## Interfaces to implement

```cpp
template <typename T>
class MpscQueue {
public:
    using value_type = T;
    static constexpr bool multi_producer = true;
    static constexpr bool multi_consumer = false;
    explicit MpscQueue(std::size_t capacity);   // power of two, >= 2
    ~MpscQueue();                                // destroys elements still queued
    [[nodiscard]] bool try_push(T&& value);      // any thread
    [[nodiscard]] std::optional<T> try_pop();    // the single consumer thread
    [[nodiscard]] std::size_t capacity() const noexcept;
};
```

Algorithm (see ADR-0011 for the orderings). Initially `slot[i].sequence = i`.

**Push:**
1. `pos = enqueue_pos_.load(relaxed)`.
2. `seq = slot[pos & mask].sequence.load(acquire)`.
3. If `seq == pos`, claim the slot with
   `enqueue_pos_.compare_exchange_weak(pos, pos + 1, relaxed)`; on success,
   construct the element and `sequence.store(pos + 1, release)`.
4. If `seq < pos`, the queue is full: return `false`.
5. Otherwise, reload `pos` and retry.

**Pop:**
1. `seq = slot[dequeue_pos_ & mask].sequence.load(acquire)`.
2. If `seq == dequeue_pos_ + 1`, move the element out and destroy it, then
   `sequence.store(dequeue_pos_ + capacity, release)` and advance
   `dequeue_pos_`.
3. Otherwise the queue is empty.

Then change `IngressQueue` in `app/include/lockstep/app/queues.hpp` to
`concurrency::MpscQueue<InboundCommand>`. The `static_assert` there guards the
producer policy.

## Acceptance criteria

1. `MpscQueue<std::unique_ptr<int>>` is added to `AllQueues`, and
   `MpscQueue<std::uint64_t>` to `MultiProducerQueues`, in
   `tests/concurrency/queue_contract_test.cpp`. Everything passes, including
   the existing 4-producer per-producer-FIFO test.
2. New `tests/concurrency/mpsc_queue_test.cpp`:
   - **Stress:** 8 producers × 500 000 items. The consumer checks that every
     item arrives exactly once and that FIFO holds per producer. Passes under
     `ctest --preset tsan`.
   - **Full-queue behaviour:** with capacity 4 and no consumer, the 5th push
     fails and the value is intact.
   - **Differential:** a single-threaded random op sequence matches
     `MutexQueue` exactly.
   - **Lifetime:** counting type, destroyed while non-empty; no leaks.
3. `IngressQueue` is switched; the full suite passes on debug, asan-ubsan and
   tsan, including `tests/determinism` and `tests/app`.
4. Every atomic operation carries an ordering comment. clang-tidy is clean.

## Files expected to change

- `exchange-core/concurrency/include/lockstep/concurrency/mpsc_queue.hpp`
- `exchange-core/tests/concurrency/mpsc_queue_test.cpp` (new), `tests/concurrency/CMakeLists.txt`
- `exchange-core/tests/concurrency/queue_contract_test.cpp` (type lists)
- `exchange-core/app/include/lockstep/app/queues.hpp` (one line)

## Out of scope

- Benchmarks (task 018). Parking/wake-up (task 007).

## Dependencies

None. Edits the same type list and `queues.hpp` as task 005 (one line each).
