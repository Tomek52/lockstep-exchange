# 18. Sentinel accumulated-cash overflow policy

- **Status:** Accepted
- **Date:** 2026-10-03

## Context

risk-sentinel tracks positions and mark-to-market PnL per
`(trader, instrument)` and aggregates PnL per trader and across the whole
exchange (task 015). Prices are integer ticks and quantities integer lots
([ADR-0005](0005-fixed-point-prices-and-quantities.md)); the sentinel holds
notionals and accumulated cash in `i128`.

A single fill's notional always fits: `|i64| × u64 < 2^127`, so no individual
`price × quantity` product can overflow `i128`. Accumulated cash is different.
Summing notionals across fills can, in principle, exceed `i128` after about
two fills at the extreme corner of the domain (`i64::MAX × u64::MAX`). ADR-0005
recorded this as a known limit: validated instrument ranges (default 1e9 ticks
and 1e6 lots, so a single notional ≤ 1e15) make it unreachable in practice,
but the arithmetic type still admits it, and task 015 was left to decide the
policy.

Three options exist for the accumulation arithmetic in `PositionBook`:

- **wrap** (`wrapping_add`): silently produces a wrong PnL, which would feed a
  wrong risk decision — the worst outcome for a safety component;
- **panic** (plain `+`, or `unwrap` on a checked result): aborts the sentinel
  process on an input that is within the `i64`/`u64` wire domain, violating
  the "`on_fill` never panics" property the task requires;
- **checked** (`checked_add` / `checked_mul`): detects the overflow and lets
  the caller decide what it means.

ADR-0013 fixes the surrounding semantics: the sentinel is a post-trade risk
monitor whose strongest action is the kill switch, and each action is emitted
once per breach, not on every subsequent fill
([idempotency](0013-risk-feedback-loop.md#adr0013-idempotency-v1)).

## <a id="adr0018-checked-accumulation-v1"></a>Decision

**Use checked accumulation, and treat an overflow as a risk breach that
engages the kill switch.**

- `PositionBook` accumulates cash and computes PnL with `checked_add` and
  `checked_mul`. The methods that can accumulate return `Result<_, Overflow>`:

  ```rust
  pub fn apply(&mut self, fill: &Fill) -> Result<&Position, Overflow>;
  pub fn trader_pnl(&self, trader: u64) -> Result<Notional, Overflow>;
  pub fn total_pnl(&self) -> Result<Notional, Overflow>;
  ```

  `Overflow` is a plain marker error; there is nothing a caller can do to
  recover a correct number, only to react safely.

- `RiskEngine::on_fill` maps any `Overflow` to a risk breach: it returns
  `RiskAction::KillSwitch { engaged: true }` with a reason naming the
  overflow, exactly once (subject to the once-per-breach rule of
  [ADR-0013](0013-risk-feedback-loop.md#adr0013-idempotency-v1)). It never
  panics and never wraps.

- The arithmetic domain is explicit: a single fill's notional cannot overflow
  (`|i64| × u64 < 2^127`), so only accumulation across fills can, and only far
  outside any validated instrument range. The policy therefore costs nothing
  on every realistic fill and only matters at the unreachable corner.

## Consequences

- The sentinel is fail-safe at the arithmetic boundary: an input it cannot
  account for correctly halts trading rather than producing a wrong PnL
  (wrap) or killing the monitor itself (panic). Halting is the sentinel's
  defined strongest response, so overflow degrades into an already-understood
  action.
- `on_fill` is total over the `i64`/`u64` fill domain and never panics, which
  task 015 pins as a property test (two fills at `i64::MAX × u64::MAX` yield
  `KillSwitch { engaged: true }`, with no panic).
- `PositionBook`'s accumulating methods carry `Result` in their signatures, so
  the possibility of overflow is visible at every call site rather than hidden
  in a comment, consistent with the project's "express failure in the type"
  rule.
- The kill switch here is engaged by the sentinel's own arithmetic, not by an
  operator or a loss limit. The reason string distinguishes it so the halt is
  auditable, matching ADR-0013's requirement that actions carry which limit
  and which values tripped them.
- This resolves the known limit left open in
  [ADR-0005](0005-fixed-point-prices-and-quantities.md); the two ADRs now
  jointly describe the fixed-point domain and its overflow behaviour.

## Alternatives considered

- **Wrapping arithmetic.** Cheapest, but a risk monitor that computes a wrong
  PnL and acts on it is worse than one that halts. Rejected outright for a
  safety component.
- **Panic on overflow.** Simple, but aborts the sentinel on an input inside
  the wire domain, breaking the no-panic property and taking the monitor
  offline exactly when the numbers are extreme. Rejected.
- **Widen beyond `i128` (e.g. `i256` or big integers).** Pushes the boundary
  further out but never removes it, and adds a dependency or hand-rolled
  arithmetic for a corner that validated ranges already make unreachable.
  Not worth the cost.
- **Saturating arithmetic (`saturating_add`).** Keeps numbers finite without
  panicking, but a saturated PnL is still a wrong number fed to a risk
  decision, with no signal that it saturated. Checked accumulation gives the
  same safety without the silent error. Rejected.
