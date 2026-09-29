# 003: Modify (cancel/replace) and duplicate client order ids

## Goal

Implement `ModifyOrder` with standard priority rules, and reject new orders
that reuse a client order id still live for the same trader.

## Context

- Rules: [domain-model.md](../architecture/domain-model.md#arch-matching-rules-v1) ("Matching rules").
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
| price change or `new_quantity > remaining` | cancel/replace: remove, then treat as an incoming order with the **same OrderId** at the new price (it may trade), rest any remainder at the tail | `OrderModified{kept_priority=false}`, `BookLevelChanged` (old level), then the matching events as in [task 002](002-matching-limit-market.md#t002-event-order-per-fill-v1) |

Add to `OrderBook`:

```cpp
/// Sets the remaining quantity of a resting order in place (priority kept).
/// Precondition: 0 < quantity < current remaining.
void reduce(OrderId id, Quantity quantity, EventBuffer& out);
/// Removes a resting order without emitting OrderCancelled (used by replace).
[[nodiscard]] std::optional<RestingOrder> take(OrderId id, EventBuffer& out);
```

<a id="t003-duplicate-client-ids-rule-v1"></a>**[t003-duplicate-client-ids-rule v1]**
**Duplicate client order ids.** `NewOrder` is rejected with
`DuplicateClientOrderId` if the same `(trader, client_order_id)` belongs to an
order currently resting on any book in the shard. Once the order is filled or
cancelled, the id may be reused. Document this choice in the
`RejectReason::DuplicateClientOrderId` comment. Keep the lookup structure
lookup-only (never iterated) to preserve determinism.

## Acceptance criteria

Tests in `exchange-core/tests/domain/modify_test.cpp`:

1. <a id="t003-reduce-keeps-priority-v1"></a>**[t003-reduce-keeps-priority v1]**
   Reduce at the same price keeps the order ahead of a later order at that
   level (checked via `front()`).
2. <a id="t003-increase-loses-priority-v1"></a>**[t003-increase-loses-priority v1]**
   Increase quantity loses priority: the order moves behind the later order.
3. <a id="t003-reprice-crossing-trades-same-id-v1"></a>**[t003-reprice-crossing-trades-same-id v1]**
   Price change to a crossing price trades immediately, with the same
   `OrderId` in `Trade::taker_order`.
4. <a id="t003-non-owner-rejected-v1"></a>**[t003-non-owner-rejected v1]**
   Modify by a non-owner → `NotOrderOwner`, book unchanged, no events.
5. <a id="t003-unknown-or-invalid-rejected-v1"></a>**[t003-unknown-or-invalid-rejected v1]**
   Modify of an unknown id → `UnknownOrder`; invalid new price or quantity →
   `InvalidPrice` / `InvalidQuantity`.
6. <a id="t003-duplicate-client-id-v1"></a>**[t003-duplicate-client-id v1]**
   Duplicate client id while resting → `DuplicateClientOrderId`. After a
   cancel, the same client id is accepted.
7. <a id="t003-determinism-test-passes-v1"></a>**[t003-determinism-test-passes v1]**
   The determinism test (`tests/determinism`) still passes.
8. <a id="t003-presets-and-tidy-pass-v1"></a>**[t003-presets-and-tidy-pass v1]**
   Presets debug, asan-ubsan and tsan pass; clang-tidy is clean.

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
