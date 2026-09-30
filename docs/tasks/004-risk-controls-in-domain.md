# 004: Risk controls in the domain: block, kill switch, link policy

## Goal

Give `RiskState` and the risk-command handlers of `ShardEngine` their real
behaviour, so that commands from risk-sentinel actually stop trading, and
replay reproduces it.

## Context

- [ADR-0013](../adr/0013-risk-feedback-loop.md): [risk loop semantics](../adr/0013-risk-feedback-loop.md#adr0013-decision-v1).
  [Commands are broadcast to every shard and journaled](../adr/0013-risk-feedback-loop.md#adr0013-commands-journaled-v1);
  [link status is a journaled input](../adr/0013-risk-feedback-loop.md#adr0013-link-status-input-v1);
  `RiskLinkPolicy` is FailOpen or FailClosed.
- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md).
- Current stubs:
  - `exchange-core/domain/src/risk_state.cpp` (all no-ops, inside a
    `NOLINTBEGIN(readability-convert-member-functions-to-static)` block that
    must be removed);
  - the `on(BlockTrader|UnblockTrader|KillSwitch|RiskLinkStatus)` handlers in
    `exchange-core/domain/src/shard_engine.cpp`.
- `OrderBook::cancel_if(predicate, reason, out)` comes from
  [task 001](001-order-book-storage.md#t001-cancel-if-by-trader-v1).

## Interfaces to implement

`RiskState` (public API already declared in `risk_state.hpp`):

- `block(t)` / `unblock(t)`: maintain the blocked-trader set. Use a sorted
  container, or a hash set that is never iterated.
- `set_kill_switch(bool)`, `set_link_connected(bool)`.
- `check_new_order(trader)`, returning the first failing check in this order:
  1. halted → `TradingHalted`;
  2. trader blocked → `TraderBlocked`;
  3. policy FailClosed and link down → `RiskUnavailable`;
  4. otherwise OK.

  The initial link state is "down". Under FailOpen that has no effect.

<a id="t004-handler-events-v1"></a>`ShardEngine` handlers:

| Command | State change | Events (in order) |
|---|---|---|
| `BlockTrader{id, t}` | block t | `OrderCancelled(TraderBlocked)` + `BookLevelChanged` for each of t's resting orders (via `cancel_if`), then `RiskCommandApplied{id}` |
| `UnblockTrader{id, t}` | unblock t | `RiskCommandApplied{id}` |
| `KillSwitch{id, true}` | halt | for each book in instrument-id order: cancel all resting orders (`CancelReason::KillSwitch`), then `InstrumentStatusChanged{halted=true}`; finally `RiskCommandApplied{id}` |
| `KillSwitch{id, false}` | resume | `InstrumentStatusChanged{halted=false}` per book, then `RiskCommandApplied{id}` |
| `RiskLinkStatus{c}` | set link | none |

[Idempotency](../adr/0013-risk-feedback-loop.md#adr0013-idempotency-v1): blocking an already-blocked trader or engaging an engaged kill
switch changes nothing and still emits `RiskCommandApplied`. A redundant
disengage (kill switch already off) likewise emits only `RiskCommandApplied`.

`ModifyOrder` is also gated by `check_new_order`. `CancelOrder` is always
allowed, even when halted.

## Acceptance criteria

Tests in `exchange-core/tests/domain/risk_controls_test.cpp`:

1. <a id="t004-blocked-trader-rejected-v1"></a>**[t004-blocked-trader-rejected v1]**
   A blocked trader's new order → `TraderBlocked`; other traders unaffected.
2. <a id="t004-block-cancels-resting-orders-v1"></a>**[t004-block-cancels-resting-orders v1]**
   Blocking cancels exactly that trader's resting orders, on both sides and
   across instruments of the shard, in the [documented order](#t004-handler-events-v1).
3. <a id="t004-unblock-accepts-again-v1"></a>**[t004-unblock-accepts-again v1]**
   After unblock, new orders are accepted again.
4. <a id="t004-kill-switch-halts-and-resumes-v1"></a>**[t004-kill-switch-halts-and-resumes v1]**
   Kill switch on: every resting order is cancelled, every instrument emits
   `InstrumentStatusChanged{true}`, and new orders → `TradingHalted`.
   Cancels still work. Off: orders are accepted again.
5. <a id="t004-fail-closed-link-policy-v1"></a>**[t004-fail-closed-link-policy v1]**
   [FailClosed](../adr/0013-risk-feedback-loop.md#adr0013-fail-closed-policy-v1): before any `RiskLinkStatus{true}` → `RiskUnavailable`; after
   link-up, accepted; after link-down, `RiskUnavailable` again. FailOpen:
   always accepted.
6. <a id="t004-duplicate-block-idempotent-v1"></a>**[t004-duplicate-block-idempotent v1]**
   [Duplicate `BlockTrader`](../adr/0013-risk-feedback-loop.md#adr0013-idempotency-v1) with the same id: second application changes
   nothing, but still acks.
7. <a id="t004-risk-ack-test-still-passes-v2"></a>**[t004-risk-ack-test-still-passes v2]**
   The existing test `ShardEngineTest.RiskCommandsAreAcknowledgedPerShard`
   still passes, updated to the full event vector.
8. <a id="t004-risk-state-nolint-removed-v1"></a>**[t004-risk-state-nolint-removed v1]**
   The `NOLINTBEGIN/END` block in `risk_state.cpp` is gone; clang-tidy is clean.
9. <a id="t004-presets-pass-v1"></a>**[t004-presets-pass v1]**
   Presets debug, asan-ubsan and tsan pass.

## Files expected to change

- `exchange-core/domain/include/lockstep/domain/risk_state.hpp`
- `exchange-core/domain/src/risk_state.cpp`
- `exchange-core/domain/src/shard_engine.cpp` (risk handlers, `on(ModifyOrder)` gate)
- `exchange-core/tests/domain/risk_controls_test.cpp` (new), `tests/domain/CMakeLists.txt`

## Out of scope

- How commands arrive over gRPC ([task 014](014-risk-client-adapter.md)) and
  how the sentinel decides (tasks [015](015-sentinel-positions-pnl.md)/[016](016-sentinel-monitor-service.md)).
- Per-instrument halts (only the global kill switch exists in v1).

## Dependencies

- **Hard:** [001](001-order-book-storage.md) ([`cancel_if`](001-order-book-storage.md#t001-cancel-if-by-trader-v1)).
- **Soft:** [002](002-matching-limit-market.md) (with matching, blocked traders' fills can be observed end to end).
