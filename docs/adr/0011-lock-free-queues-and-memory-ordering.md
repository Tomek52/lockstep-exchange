# 11. Lock-free queue designs and memory-ordering policy

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

The runtime (ADR-0003) needs two queue shapes:

- **Ingress, MPSC:** several gRPC threads plus the risk client push into one
  shard.
- **Egress, SPSC:** one shard pushes to the publisher.

Both are bounded (back-pressure instead of unbounded memory) and must not
block producers. Atomics are the most error-prone code in the repository.
Overly strong orderings (`seq_cst` everywhere) hide the reasoning and cost
fences on weakly-ordered CPUs. Overly weak ones are bugs that tests catch
only by luck.

## Decision

**Interfaces first.** All queues satisfy the `ConcurrentQueue` concept
(`queue_concepts.hpp`):

- `try_push(T&&) -> bool`, which moves from its argument only on success;
- `try_pop() -> optional<T>`;
- `capacity()`;
- static `multi_producer` / `multi_consumer` flags.

`MultiProducerQueue` is the policy concept the ingress requires.
`app/queues.hpp` is the single place that picks the concrete types, and
`static_assert`s the policy.

**SPSC** (task 005): a ring buffer with power-of-two capacity.

- `head_` (consumer-written) and `tail_` (producer-written) live on separate
  cache lines.
- Each side keeps a private cached copy of the other side's index and
  refreshes it only when the cache says full/empty.
- Orderings:
  - producer: stores the element, then `tail_.store(release)`;
  - consumer: `tail_.load(acquire)`, reads the element, then
    `head_.store(release)`;
  - producer: `head_.load(acquire)` before reusing a slot;
  - each side reads its *own* index `relaxed`, since only it writes that
    index.

**MPSC** (task 006): Dmitry Vyukov's bounded queue.

- Each slot has an atomic sequence number.
- Producers `enqueue_pos_.compare_exchange_weak(relaxed)` to claim a slot
  after `slot.sequence.load(acquire)` shows it free. They write the element,
  then `slot.sequence.store(pos + 1, release)`.
- The consumer (single) reads `slot.sequence.load(acquire)`, moves the
  element out, then `slot.sequence.store(pos + capacity, release)`.
- `dequeue_pos_` is consumer-private (no atomic needed).

**Policy.**

1. Use the **weakest ordering that is correct**. `seq_cst` is allowed only
   with a comment explaining why nothing weaker works.
2. **Every** non-`seq_cst` atomic operation carries a comment naming what it
   synchronises with, or why no synchronisation is needed. Examples:
   `// acquire: pairs with release in push(); makes the element visible`,
   `// relaxed: only this thread writes head_`.
3. **Cache lines.** Independently written fields are separated with
   `alignas(concurrency::cache_line_size)`, which is 128, not 64. Intel's
   adjacent-line prefetcher pulls 64-byte lines in pairs, and Apple/ARM big
   cores use 128-byte lines. We don't use
   `std::hardware_destructive_interference_size`: GCC warns it is not
   ABI-stable (`-Winterference-size`).
4. **Proof obligations for a queue PR:**
   - typed contract tests (`tests/concurrency/queue_contract_test.cpp`) pass;
   - a multi-threaded stress test passes under the `tsan` preset;
   - a differential test against `MutexQueue` (the reference model) passes;
   - a benchmark shows the latency percentiles.
5. **Until then,** `MutexQueue`, a bounded deque behind a mutex, carries the
   walking skeleton. The lock-free headers already fix their interface and
   layout, and `static_assert` with a pointer to their task spec if
   instantiated.

## Consequences

- Memory-ordering reasoning is reviewable line by line instead of implied.
- TSan validates the happens-before edges we claim. It cannot prove the
  *absence* of every reordering on weak hardware, so reviewers check the
  comments against this ADR.
- Swapping a queue implementation is a one-line change in `app/queues.hpp`,
  guarded by concepts.
