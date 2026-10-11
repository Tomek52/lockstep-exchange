# 21. Sentinel kill switch trips on gross loss, not net total PnL

- **Status:** Proposed
- **Date:** 2026-10-11
- **Amends:** [task 015](../tasks/015-sentinel-positions-pnl.md) rule "engage the kill switch when `total_pnl < -kill_switch_loss`"

## Context

[Task 015](../tasks/015-sentinel-positions-pnl.md) specified the kill switch as
`total_pnl < -kill_switch_loss`, where `total_pnl` is the sum of every trader's
mark-to-market PnL. The documentation of the limit (`RiskLimits`, the
`--kill-switch-loss` flag) calls it the "aggregate loss across all traders".

Task 019 (the end-to-end kill-switch scenario) found that this metric can never
be reached by trading. Every trade is reported to the sentinel from both sides
(`encode_execution_reports` emits a taker and a maker report). Per instrument,
the cash of the two sides cancels and so do the net positions, so
`Σ cash + mark × Σ net = 0` for any mark. The sum of all PnLs is therefore
identically zero in an exchange whose trades are all reported: a loss for one
trader is exactly a gain for another.

This was checked on a running stack: with `--kill-switch-loss 0`, about 1 400
random orders from five traders (hundreds of trades) produced no kill switch.
The only way to trip the rule was an `i128` overflow
([ADR-0018](0018-sentinel-overflow-policy.md)), or a unit test feeding
one-sided fills that the real exchange never produces. The kill switch on loss
was dead code.

## <a id="adr0021-gross-loss-kill-switch-v1"></a>Decision

**The kill switch engages when the gross loss exceeds `kill_switch_loss`.**

- *Gross loss* is the sum, over all traders whose mark-to-market PnL is
  negative, of the absolute value of that PnL:
  `gross_loss = Σ_t max(0, −pnl_t)`. Traders in profit do not offset it.
- The comparison is strict (`gross_loss > kill_switch_loss`), as before:
  a gross loss exactly equal to the limit does not trip it.
- `PositionBook::gross_loss() -> Result<Notional, Overflow>` computes it with
  checked arithmetic and visits traders in ascending id, like `total_pnl`
  ([ADR-0004](0004-deterministic-replay-via-per-shard-journal.md) style
  determinism near the overflow boundary). An overflow still engages the kill
  switch ([ADR-0018](0018-sentinel-overflow-policy.md)), with a reason naming
  the gross-loss computation.
- The kill switch is still emitted at most once
  ([ADR-0013](0013-risk-feedback-loop.md#adr0013-idempotency-v1)).
- The reason string names the metric and the values, for example
  `"gross loss 6600 > 5000 (kill switch)"`.
- `PositionBook::total_pnl` stays (it is the zero-sum invariant the property
  tests rely on); the engine just no longer uses it for decisions.
- The per-trader rule (`trader_pnl < -max_trader_loss` blocks the trader) is
  unchanged.
- The CLI flag and the `RiskLimits` field keep their names and their unit
  (tick-lots); only the documented meaning is made precise.

## Alternatives considered

- **Keep `total_pnl` and accept that the loss kill switch cannot fire.**
  Leaves a configured safety limit that silently never acts. Rejected.
- **Net the PnL only over traders that are not market makers or the
  exchange.** The sentinel has no notion of such traders and the task does
  not introduce one; it would need a reference-data channel to the sentinel.
  Rejected as out of proportion.
- **Use the largest single-trader loss.** That is already the per-trader
  block; a kill switch is meant to catch many moderate losses that no single
  limit sees. Rejected.
- **Use realised loss only.** Needs average-price or lot accounting that
  ([task 015](../tasks/015-sentinel-positions-pnl.md)) deliberately avoided
  (`cash + net × mark`). Rejected.

## Consequences

- The kill switch can now be reached by trading, so the cross-process
  scenario of [task 019](../tasks/019-e2e-kill-switch.md) can exercise it.
- Gross loss is not a net-worth measure: with noisy but balanced flow it can
  grow while the market as a whole is flat, because each trader's unrealised
  loss counts and the offsetting gains do not. Operators must set
  `--kill-switch-loss` well above the noise of normal flow; the end-to-end
  scenario uses limits an order of magnitude above its random traders'
  swings.
- Computing it visits every position on each fill, like `total_pnl` did
  (see the `TODO(perf)` in `RiskEngine::on_fill`). Not measured.
- Enforced by unit tests in `engine.rs` (gross loss trips, profits do not
  offset, equality does not trip, overflow still trips) and by the end-to-end
  script `scripts/e2e-kill-switch.sh`.
