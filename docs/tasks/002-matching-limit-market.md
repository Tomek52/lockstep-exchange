# 002: Price-time matching for limit and market orders

## Goal

Make `ShardEngine` match incoming `NewOrder`s against the opposite side of the
book with price-time priority, emitting trades and level updates, and rest
or cancel the remainder according to order type and time in force.

## Context

- Domain model and target rules:
  [docs/architecture/domain-model.md](../architecture/domain-model.md)
  (["Matching rules"](../architecture/domain-model.md#arch-matching-rules-v1)).
- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md): the
  output must be [a pure function of the command sequence](../adr/0004-deterministic-replay-via-per-shard-journal.md#adr0004-determinism-property-v1).
- [ADR-0005](../adr/0005-fixed-point-prices-and-quantities.md): [integer ticks](../adr/0005-fixed-point-prices-and-quantities.md#adr0005-price-integer-ticks-v1)
  and [lots](../adr/0005-fixed-point-prices-and-quantities.md#adr0005-quantity-integer-lots-v1) only.
- `ShardEngine::on(const NewOrder&, EventBuffer&)` in
  `exchange-core/domain/src/shard_engine.cpp` currently validates, emits
  `OrderAccepted` and stops (`TODO(task-002)`).
- The book primitives `front(Side)`, `reduce_front(Side, Quantity, EventBuffer&)`
  and `rest(...)` come from [task 001](001-order-book-storage.md#t001-book-primitives-v1).

## <a id="t002-match-interface-v1"></a>Interfaces to implement

```cpp
// exchange-core/domain/include/lockstep/domain/matching.hpp
namespace lockstep::domain {
/// Matches `incoming` against `book`'s opposite side. Appends Trade and
/// BookLevelChanged events. Returns the unfilled quantity.
[[nodiscard]] Quantity match(OrderBook& book, const OrderAccepted& incoming, EventBuffer& out);
}
```

`ShardEngine::on(NewOrder)` becomes:

1. Validate. Unchanged; rejections emit nothing.
2. Assign an id and emit `OrderAccepted`.
3. `remaining = match(...)`.
4. If `remaining > 0`:
   - GTC limit → `book.rest(...)` (emits `BookLevelChanged` on own side);
   - IOC limit or market → emit
     `OrderCancelled{..., remaining, CancelReason::ImmediateOrCancel}`.
5. Return `CommandOutcome{id}`.

Matching rules:

- A buy limit at price P matches asks with price ≤ P; a sell limit at price
  P matches bids with price ≥ P. A market order matches any price.
- Best price first; within a price, the oldest resting order first (FIFO).
- Trade price = the **resting** order's price.
- <a id="t002-fill-event-order-v1"></a>**Event order per fill:** `Trade{instrument, price, qty, aggressor_side,
  maker_order, maker_trader, taker_order, taker_trader}`, then the maker
  level's `BookLevelChanged` (emitted by
  [`reduce_front`](001-order-book-storage.md#t001-reduce-front-partial-v1)).
- Self-trade is allowed (documented simplification).

## Acceptance criteria

New tests in `exchange-core/tests/domain/matching_test.cpp`. Each asserts the
complete event sequence, not just counts:

1. <a id="t002-non-crossing-limit-rests-v1"></a>**[t002-non-crossing-limit-rests v1]**
   **Non-crossing** limit rests: `OrderAccepted`, then `BookLevelChanged`
   (own side), and no `Trade`.
2. <a id="t002-full-fill-single-order-v1"></a>**[t002-full-fill-single-order v1]**
   **Full fill** against one resting order: `Trade` at the maker's price,
   then `BookLevelChanged` with quantity 0. The book is empty afterwards.
3. <a id="t002-partial-fill-remainder-rests-v1"></a>**[t002-partial-fill-remainder-rests v1]**
   **Partial fill, remainder rests:** buy 10 @ 101 vs ask 4 @ 100 →
   `Trade(4 @ 100)`, ask level removed, 6 rests on the bid at 101.
4. <a id="t002-sweep-levels-best-first-v1"></a>**[t002-sweep-levels-best-first v1]**
   **Sweep across levels, best first:** asks 5 @ 100, 5 @ 101, 5 @ 102;
   buy 12 @ 102 → trades at 100, 101 and 102 (2 lots), in that order.
5. <a id="t002-fifo-within-level-v1"></a>**[t002-fifo-within-level v1]**
   **FIFO within a level:** two asks at 100 (ids A then B); buy 1 fills A,
   not B.
6. <a id="t002-price-improvement-v1"></a>**[t002-price-improvement v1]**
   **Price improvement:** buy @ 105 vs ask @ 100 trades at 100.
7. <a id="t002-market-order-empty-book-v1"></a>**[t002-market-order-empty-book v1]**
   **Market order on an empty book:** `OrderAccepted`, then
   `OrderCancelled(ImmediateOrCancel)` for the full quantity.
8. <a id="t002-ioc-remainder-cancelled-v1"></a>**[t002-ioc-remainder-cancelled v1]**
   **IOC remainder** is cancelled, never rests.
9. <a id="t002-random-commands-deterministic-v1"></a>**[t002-random-commands-deterministic v1]**
   **Determinism:** the same 1 000 random commands (fixed seed) applied to
   two fresh engines produce identical event vectors.
10. <a id="t002-existing-tests-pass-v1"></a>**[t002-existing-tests-pass v1]**
    **Existing tests:** `tests/domain/*` and `tests/determinism/*` pass.
11. <a id="t002-presets-and-tidy-pass-v1"></a>**[t002-presets-and-tidy-pass v1]**
    **Presets:** debug, asan-ubsan and tsan pass; clang-tidy is clean.

## Files expected to change

- `exchange-core/domain/include/lockstep/domain/matching.hpp` (new)
- `exchange-core/domain/src/matching.cpp` (new)
- `exchange-core/domain/src/shard_engine.cpp` (`on(NewOrder)` only)
- `exchange-core/domain/CMakeLists.txt` (add source)
- `exchange-core/tests/domain/matching_test.cpp` (new) and `tests/domain/CMakeLists.txt`

## Out of scope

- Modify semantics and duplicate client ids
  ([task 003](003-cancel-modify-duplicates.md)).
- Blocked traders and halts ([task 004](004-risk-controls-in-domain.md)).
- Self-trade prevention (future task, needs an ADR).

## Dependencies

- **Hard:** [001](001-order-book-storage.md) ([book primitives](001-order-book-storage.md#t001-book-primitives-v1)).
