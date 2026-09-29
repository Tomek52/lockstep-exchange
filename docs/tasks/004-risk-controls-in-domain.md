# 004: Risk controls in the domain: block, kill switch, link policy

## Goal

Give `RiskState` and the risk-command handlers of `ShardEngine` their real
behaviour, so that commands from risk-sentinel actually stop trading, and
replay reproduces it.

## Context

- [ADR-0013](../adr/0013-risk-feedback-loop.md): risk loop semantics.
  Commands are broadcast to every shard and journaled; link status is a
  journaled input; `RiskLinkPolicy` is FailOpen or FailClosed.
- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md).
- Current stubs:
  - `exchange-core/domain/src/risk_state.cpp` (all no-ops, inside a
    `NOLINTBEGIN(readability-convert-member-functions-to-static)` block that
    must be removed);
  - the `on(BlockTrader|UnblockTrader|KillSwitch|RiskLinkStatus)` handlers in
    `exchange-core/domain/src/shard_engine.cpp`.
- `OrderBook::cancel_if(predicate, reason, out)` comes from task 001.

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

`ShardEngine` handlers:

| Command | State change | Events (in order) |
|---|---|---|
| `BlockTrader{id, t}` | block t | `OrderCancelled(TraderBlocked)` + `BookLevelChanged` for each of t's resting orders (via `cancel_if`), then `RiskCommandApplied{id}` |
| `UnblockTrader{id, t}` | unblock t | `RiskCommandApplied{id}` |
| `KillSwitch{id, true}` | halt | for each book in instrument-id order: cancel all resting orders (`CancelReason::KillSwitch`), then `InstrumentStatusChanged{halted=true}`; finally `RiskCommandApplied{id}` |
| `KillSwitch{id, false}` | resume | `InstrumentStatusChanged{halted=false}` per book, then `RiskCommandApplied{id}` |
| `RiskLinkStatus{c}` | set link | none |

Idempotency: blocking an already-blocked trader or engaging an engaged kill
switch changes nothing and still emits `RiskCommandApplied`. A redundant
disengage (kill switch already off) likewise emits only `RiskCommandApplied`.

`ModifyOrder` is also gated by `check_new_order`. `CancelOrder` is always
allowed, even when halted.

## Acceptance criteria

Tests in `exchange-core/tests/domain/risk_controls_test.cpp`.

Each criterion is an OpenFastTrace requirement. Its ID is the stable name
that tests, code and other documents refer to; the number only gives the
reading order. Conventions: [CLAUDE.md](../../CLAUDE.md#7-requirement-tracing-openfasttrace).

### AC 1: A blocked trader's new orders are rejected
`req~risk-controls.blocked-trader-new-order-rejected~1`

A blocked trader's new order → `TraderBlocked`; other traders unaffected.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 2: Blocking cancels exactly that trader's resting orders
`req~risk-controls.block-cancels-traders-resting-orders~1`

Blocking cancels exactly that trader's resting orders, on both sides and
across instruments of the shard, in the documented order.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Depends:
- [req~order-book-storage.cancel-if-by-trader~1](001-order-book-storage.md#ac-2e-cancel_if-by-trader-cancels-in-the-documented-order)

Needs: impl, utest

### AC 3: After unblock, new orders are accepted again
`req~risk-controls.unblock-accepts-new-orders~1`

After unblock, new orders are accepted again.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: impl, utest

### AC 4: The kill switch cancels, halts and resumes
`req~risk-controls.kill-switch-cancels-halts-and-resumes~1`

Kill switch on: every resting order is cancelled, every instrument emits
`InstrumentStatusChanged{true}`, and new orders → `TradingHalted`.
Cancels still work. Off: orders are accepted again.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Depends:
- [req~order-book-storage.cancel-if-by-trader~1](001-order-book-storage.md#ac-2e-cancel_if-by-trader-cancels-in-the-documented-order)

Needs: impl, utest

### AC 5: FailClosed gates new orders on the risk link, FailOpen does not
`req~risk-controls.fail-closed-gates-on-link-status~1`

FailClosed: before any `RiskLinkStatus{true}` → `RiskUnavailable`; after
link-up, accepted; after link-down, `RiskUnavailable` again. FailOpen:
always accepted.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: dsn, utest

### AC 6: A duplicate BlockTrader changes nothing but still acks
`req~risk-controls.duplicate-block-trader-only-acks~1`

Duplicate `BlockTrader` with the same id: second application changes
nothing, but still acks.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: dsn, utest

### AC 7: Risk commands are acknowledged per shard
`req~risk-controls.risk-commands-acknowledged-per-shard~1`

The existing test `ShardEngineTest.RiskCommandsAreAcknowledgedPerShard`
still passes, updated to the full event vector.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: dsn, utest

### AC 8: No NOLINT block in risk_state.cpp, clang-tidy clean
`req~risk-controls.no-nolint-in-risk-state~1`

The `NOLINTBEGIN/END` block in `risk_state.cpp` is gone; clang-tidy is clean.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: bld

### AC 9: Presets pass
`req~risk-controls.presets-pass~1`

Presets debug, asan-ubsan and tsan pass.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Needs: bld

## Files expected to change

- `exchange-core/domain/include/lockstep/domain/risk_state.hpp`
- `exchange-core/domain/src/risk_state.cpp`
- `exchange-core/domain/src/shard_engine.cpp` (risk handlers, `on(ModifyOrder)` gate)
- `exchange-core/tests/domain/risk_controls_test.cpp` (new), `tests/domain/CMakeLists.txt`

## Out of scope

- How commands arrive over gRPC (task 014) and how the sentinel decides
  (tasks 015/016).
- Per-instrument halts (only the global kill switch exists in v1).

## Dependencies

- **Hard:** 001 (`cancel_if`).
- **Soft:** 002 (with matching, blocked traders' fills can be observed end to end).
