# 002: Price-time matching for limit and market orders

## Goal

Make `ShardEngine` match incoming `NewOrder`s against the opposite side of the
book with price-time priority, emitting trades and level updates, and rest
or cancel the remainder according to order type and time in force.

## Context

- Domain model and target rules:
  [docs/architecture/domain-model.md](../architecture/domain-model.md)
  ("Matching rules").
- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md): the
  output must be a pure function of the command sequence.
- [ADR-0005](../adr/0005-fixed-point-prices-and-quantities.md): integer ticks
  and lots only.
- `ShardEngine::on(const NewOrder&, EventBuffer&)` in
  `exchange-core/domain/src/shard_engine.cpp` currently validates, emits
  `OrderAccepted` and stops (`TODO(task-002)`).
- The book primitives `front(Side)`, `reduce_front(Side, Quantity, EventBuffer&)`
  and `rest(...)` come from task 001.

## Interfaces to implement

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
- **Event order per fill:** `Trade{instrument, price, qty, aggressor_side,
  maker_order, maker_trader, taker_order, taker_trader}`, then the maker
  level's `BookLevelChanged` (emitted by `reduce_front`).
- Self-trade is allowed (documented simplification).

## Acceptance criteria

New tests in `exchange-core/tests/domain/matching_test.cpp`. Each asserts the
complete event sequence, not just counts.

Each criterion is an OpenFastTrace requirement. Its ID is the stable name
that tests, code and other documents refer to; the number only gives the
reading order. Conventions: [CLAUDE.md](../../CLAUDE.md#7-requirement-tracing-openfasttrace).

### AC 1: A non-crossing limit order rests
`req~matching.non-crossing-limit-rests~1`

**Non-crossing** limit rests: `OrderAccepted`, then `BookLevelChanged`
(own side), and no `Trade`.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2: A full fill against one resting order empties the book
`req~matching.full-fill-against-one-order~1`

**Full fill** against one resting order: `Trade` at the maker's price, then
`BookLevelChanged` with quantity 0. The book is empty afterwards.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Depends:
- [req~order-book-storage.reduce-front-full~1](001-order-book-storage.md#ac-2d-full-reduce_front-removes-the-order-then-the-level)

Needs: impl, utest

### AC 3: A partial fill rests the remainder
`req~matching.partial-fill-remainder-rests~1`

**Partial fill, remainder rests:** buy 10 @ 101 vs ask 4 @ 100 →
`Trade(4 @ 100)`, ask level removed, 6 rests on the bid at 101.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Depends:
- [req~order-book-storage.reduce-front-partial~1](001-order-book-storage.md#ac-2c-partial-reduce_front-shrinks-the-order-and-its-level)

Needs: impl, utest

### AC 4: A sweep across levels takes the best price first
`req~matching.sweep-levels-best-price-first~1`

**Sweep across levels, best first:** asks 5 @ 100, 5 @ 101, 5 @ 102;
buy 12 @ 102 → trades at 100, 101 and 102 (2 lots), in that order.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Depends:
- [req~order-book-storage.reduce-front-full~1](001-order-book-storage.md#ac-2d-full-reduce_front-removes-the-order-then-the-level)

Needs: impl, utest

### AC 5: FIFO within a price level
`req~matching.fifo-within-level~1`

**FIFO within a level:** two asks at 100 (ids A then B); buy 1 fills A,
not B.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 6: Price improvement trades at the maker's price
`req~matching.price-improvement-at-maker-price~1`

**Price improvement:** buy @ 105 vs ask @ 100 trades at 100.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 7: A market order on an empty book is cancelled
`req~matching.market-order-on-empty-book-cancelled~1`

**Market order on an empty book:** `OrderAccepted`, then
`OrderCancelled(ImmediateOrCancel)` for the full quantity.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 8: An IOC remainder is cancelled, never rested
`req~matching.ioc-remainder-cancelled~1`

**IOC remainder** is cancelled, never rests.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 9: Identical command sequences produce identical events
`req~matching.identical-commands-identical-events~1`

**Determinism:** the same 1 000 random commands (fixed seed) applied to
two fresh engines produce identical event vectors.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: dsn, utest

### AC 10: Existing domain and determinism tests pass
`req~matching.existing-domain-and-determinism-tests-pass~1`

**Existing tests:** `tests/domain/*` and `tests/determinism/*` pass.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: bld

### AC 11: Presets pass and clang-tidy is clean
`req~matching.presets-and-clang-tidy-clean~1`

**Presets:** debug, asan-ubsan and tsan pass; clang-tidy is clean.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: bld

## Files expected to change

- `exchange-core/domain/include/lockstep/domain/matching.hpp` (new)
- `exchange-core/domain/src/matching.cpp` (new)
- `exchange-core/domain/src/shard_engine.cpp` (`on(NewOrder)` only)
- `exchange-core/domain/CMakeLists.txt` (add source)
- `exchange-core/tests/domain/matching_test.cpp` (new) and `tests/domain/CMakeLists.txt`

## Out of scope

- Modify semantics and duplicate client ids (task 003).
- Blocked traders and halts (task 004).
- Self-trade prevention (future task, needs an ADR).

## Dependencies

- **Hard:** 001 (book primitives).
