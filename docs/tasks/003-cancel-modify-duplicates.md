# 003: Modify (cancel/replace) and duplicate client order ids

## Goal

Implement `ModifyOrder` with standard priority rules, and reject new orders
that reuse a client order id still live for the same trader.

## Context

- Rules: [domain-model.md](../architecture/domain-model.md) ("Matching rules").
- `ShardEngine::on(const ModifyOrder&, ...)` in
  `exchange-core/domain/src/shard_engine.cpp` validates, checks
  existence/ownership, and returns `UnknownOrder` (`TODO(task-003)`).
- Matching (`match()` from task 002) is reused for aggressive re-pricing.
- Wire: `ModifyOrderRequest{trader_id, instrument_id, order_id,
  new_price_ticks, new_quantity}` in `proto/lockstep/v1/order_entry.proto`.

## Interfaces to implement

Modify semantics (`new_quantity` is the new *total remaining* quantity):

| Change | Behaviour | Events |
|---|---|---|
| same price, `new_quantity < remaining` | reduce in place, **keeps priority** | `OrderModified{kept_priority=true}`, `BookLevelChanged` |
| same price, `new_quantity == remaining` | no-op, accepted | `OrderModified{kept_priority=true}` |
| price change or `new_quantity > remaining` | cancel/replace: remove, then treat as an incoming order with the **same OrderId** at the new price (it may trade), rest any remainder at the tail | `OrderModified{kept_priority=false}`, `BookLevelChanged` (old level), then the matching events as in task 002 |

Add to `OrderBook`:

```cpp
/// Sets the remaining quantity of a resting order in place (priority kept).
/// Precondition: 0 < quantity < current remaining.
void reduce(OrderId id, Quantity quantity, EventBuffer& out);
/// Removes a resting order without emitting OrderCancelled (used by replace).
[[nodiscard]] std::optional<RestingOrder> take(OrderId id, EventBuffer& out);
```

**Duplicate client order ids.** `NewOrder` is rejected with
`DuplicateClientOrderId` if the same `(trader, client_order_id)` belongs to an
order currently resting on any book in the shard. Once the order is filled or
cancelled, the id may be reused. Document this choice in the
`RejectReason::DuplicateClientOrderId` comment. Keep the lookup structure
lookup-only (never iterated) to preserve determinism.

## Acceptance criteria

Tests in `exchange-core/tests/domain/modify_test.cpp`.

Each criterion is an OpenFastTrace requirement. Its ID is the stable name
that tests, code and other documents refer to; the number only gives the
reading order. Conventions: [CLAUDE.md](../../CLAUDE.md#7-requirement-tracing-openfasttrace).

### AC 1: Reducing quantity at the same price keeps priority
`req~modify.reduce-keeps-priority~1`

Reduce at the same price keeps the order ahead of a later order at that
level (checked via `front()`).

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2: Increasing quantity loses priority
`req~modify.increase-loses-priority~1`

Increase quantity loses priority: the order moves behind the later order.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 3: A crossing price change trades with the same order id
`req~modify.crossing-price-change-trades-with-same-id~1`

Price change to a crossing price trades immediately, with the same
`OrderId` in `Trade::taker_order`.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Depends:
- [req~matching.partial-fill-remainder-rests~1](002-matching-limit-market.md#ac-3-a-partial-fill-rests-the-remainder)

Needs: impl, utest

### AC 4: A modify by a non-owner is rejected
`req~modify.non-owner-rejected~1`

Modify by a non-owner → `NotOrderOwner`, book unchanged, no events.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 5: A modify of an unknown order or to invalid values is rejected
`req~modify.unknown-or-invalid-modify-rejected~1`

Modify of an unknown id → `UnknownOrder`; invalid new price or quantity →
`InvalidPrice` / `InvalidQuantity`.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 6: A duplicate client order id is rejected while the order rests
`req~modify.duplicate-client-id-rejected-while-resting~1`

Duplicate client id while resting → `DuplicateClientOrderId`. After a
cancel, the same client id is accepted.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 7: The determinism test still passes
`req~modify.determinism-test-still-passes~1`

The determinism test (`tests/determinism`) still passes.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: itest

### AC 8: Presets pass and clang-tidy is clean
`req~modify.presets-and-clang-tidy-clean~1`

Presets debug, asan-ubsan and tsan pass; clang-tidy is clean.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: bld

## Files expected to change

- `exchange-core/domain/src/shard_engine.cpp` (`on(ModifyOrder)`, dup check in `on(NewOrder)`)
- `exchange-core/domain/include/lockstep/domain/shard_engine.hpp` (private members)
- `exchange-core/domain/include/lockstep/domain/order_book.hpp`, `src/order_book.cpp` (`reduce`, `take`)
- `exchange-core/tests/domain/modify_test.cpp` (new), `tests/domain/CMakeLists.txt`

## Out of scope

- Wire changes (none are needed).
- Order amendment of side or instrument (not supported: cancel and resubmit).

## Dependencies

- **Hard:** 002.
