# 001: Order book storage: pooled FIFO levels and O(1) cancel

## Goal

Replace the skeleton's naive order storage in `OrderBook` with a structure
suitable for a matching engine:

- price levels in `flat_map`;
- each level a FIFO of orders drawn from a per-book pool, with an intrusive
  doubly-linked list;
- an id → slot index so `find` and `cancel` are O(1).

Observable behaviour must not change. Add the two primitives the matcher
(task 002) and risk controls (task 004) need.

## Context

- [ADR-0003](../adr/0003-single-writer-sharding.md): a book has exactly one
  writer thread, so no synchronisation.
- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md): no
  iteration over hash containers may influence output order.
- [ADR-0009](../adr/0009-toolchain-baseline-and-feature-fallbacks.md): use
  `lockstep::domain::flat_map` (fallback on libstdc++ 14). Follow the
  portability rules in `flat_map.hpp`.
- Current code: `exchange-core/domain/include/lockstep/domain/order_book.hpp`,
  `exchange-core/domain/src/order_book.cpp`.
  - `Level` is `{Quantity total; std::vector<RestingOrder> orders;}`.
  - `find` and `cancel` scan every level.
- Existing tests that must keep passing unchanged:
  `exchange-core/tests/domain/order_book_test.cpp`.
- Domain rules: no `<thread>`, `<atomic>`, `<chrono>`, I/O. Enforced by
  `ctest -L architecture`.

## Interfaces to implement

Keep every existing public member of `OrderBook` with the same signature and
semantics. Add:

```cpp
/// Oldest order at the best price level of `side`, or nullptr if that side is empty.
[[nodiscard]] const RestingOrder* front(Side side) const noexcept;

/// Reduces the remaining quantity of the order returned by front(side) by
/// `quantity` (precondition: 0 < quantity <= front(side)->remaining). Removes
/// the order when it reaches zero and the level when it becomes empty.
/// Emits exactly one BookLevelChanged for the affected level.
void reduce_front(Side side, Quantity quantity, EventBuffer& out);

/// Cancels every resting order for which `predicate` returns true, in
/// deterministic order: bids best-to-worst then asks best-to-worst, FIFO
/// within a level. Emits OrderCancelled for each order and one
/// BookLevelChanged per touched level. Returns the number cancelled.
std::size_t cancel_if(std::function_ref<bool(const RestingOrder&)> predicate, // or a template
                      CancelReason reason, EventBuffer& out);
```

`std::function_ref` is C++26 and not in libstdc++ 14. Use a constrained
template (`std::predicate<const RestingOrder&> Pred`) instead, implemented in
the header.

Internal structure (a suggestion; any structure meeting the criteria is fine):

- `OrderPool`: `std::vector<Node>` plus a free list of indices. A `Node`
  holds a `RestingOrder` and `prev`/`next` indices. It never shrinks.
- `Level`: `{Quantity total; std::uint32_t count; Index head; Index tail;}`.
- `std::unordered_map<OrderId, Index, StrongIntHash>`: used only for lookup,
  never iterated.

## Acceptance criteria

Each criterion is an OpenFastTrace requirement. Its ID is the stable name
that tests, code and other documents refer to; the number only gives the
reading order. Conventions: [CLAUDE.md](../../CLAUDE.md#7-requirement-tracing-openfasttrace).

### AC 1: Existing order book tests pass unmodified
`req~order-book-storage.existing-book-tests-pass-unmodified~1`

All tests in `tests/domain/order_book_test.cpp` pass unmodified.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: utest

### AC 2a: Cancelling a middle order preserves FIFO
`req~order-book-storage.cancel-middle-preserves-fifo~1`

New test in `tests/domain/order_book_storage_test.cpp`: cancelling the
middle order of three at one level preserves FIFO order of the other two,
checked via successive `front()`/`reduce_front()`.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2b: find() locates an order among many
`req~order-book-storage.find-among-many-orders~1`

New test in `tests/domain/order_book_storage_test.cpp`: `find()` returns the
correct order among 10 000 resting orders spread over 100 price levels on
both sides.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2c: Partial reduce_front shrinks the order and its level
`req~order-book-storage.reduce-front-partial~1`

New test in `tests/domain/order_book_storage_test.cpp`: `reduce_front`
partial: remaining decreases, the level total decreases, and one
`BookLevelChanged` carries the new total.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2d: Full reduce_front removes the order, then the level
`req~order-book-storage.reduce-front-full~1`

New test in `tests/domain/order_book_storage_test.cpp`: `reduce_front` full:
the order is removed; with the last order, the level disappears
(`best_price` moves to the next level).

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2e: cancel_if by trader cancels in the documented order
`req~order-book-storage.cancel-if-by-trader~1`

New test in `tests/domain/order_book_storage_test.cpp`: `cancel_if` by
trader: the correct orders are removed, events come in the documented order,
and the return value is the count.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2f: Pool slots are reused after cancel
`req~order-book-storage.pool-slots-reused~1`

New test in `tests/domain/order_book_storage_test.cpp`: slot reuse: rest
1 000 and cancel 1 000, repeated 10 times. The pool's capacity does not grow
after the first round (expose `pool_capacity()` for tests, or test via a
debug accessor).

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 3: Presets pass and clang-tidy is clean
`req~order-book-storage.presets-and-clang-tidy-clean~1`

`ctest --preset debug`, `--preset asan-ubsan` and `--preset tsan` pass;
`scripts/run-clang-tidy.sh` is clean.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: bld

## Files expected to change

- `exchange-core/domain/include/lockstep/domain/order_book.hpp`
- `exchange-core/domain/src/order_book.cpp`
- `exchange-core/domain/include/lockstep/domain/order_pool.hpp` (new, optional)
- `exchange-core/tests/domain/order_book_storage_test.cpp` (new)
- `exchange-core/tests/domain/CMakeLists.txt` (add the source)

## Out of scope

- Matching (task 002), modify (task 003), risk semantics (task 004).
- Benchmarks (task 018). Keep the code benchmark-friendly: no allocation per
  operation after warm-up.

## Dependencies

None. Tasks 002 and 004 build on `front`, `reduce_front` and `cancel_if`.
