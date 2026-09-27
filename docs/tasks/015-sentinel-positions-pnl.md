# 015: Sentinel positions, PnL and limit engine (Rust)

## Goal

Implement risk-sentinel's pure decision logic:

- positions per `(trader, instrument)`;
- mark-to-market PnL;
- limits that produce `RiskAction`s exactly once per breach.

No I/O and no async.

## Context

- Crate: `rust/crates/risk-sentinel`. Modules:
  - `positions.rs` (`Position` with `apply`/`pnl`, tested);
  - `limits.rs` (`RiskLimits`);
  - `engine.rs` (`Fill`, `RiskAction`, `RiskEngine::on_fill`, currently a
    stub returning nothing).
- [ADR-0005](../adr/0005-fixed-point-prices-and-quantities.md): integer ticks
  and lots; notionals in `i128`. A single fill's notional always fits, but
  accumulated cash can overflow after about two fills of extreme size. **This
  task decides the policy.** The recommendation is checked accumulation
  (`checked_add`), with an overflow treated as a breach that engages the kill
  switch. Record the decision as **ADR-0016** and link it from ADR-0005's
  consequences.
- [ADR-0013](../adr/0013-risk-feedback-loop.md): actions must be idempotent.
  Emit each action once per breach, not on every later fill.
- Lints: clippy pedantic with `-D warnings` (workspace `Cargo.toml`). The
  `proptest` dev-dependency is available.

## Interfaces to implement

```rust
pub struct PositionBook { /* HashMap<(u64, u32), Position>, marks: HashMap<u32, Ticks> */ }
impl PositionBook {
    pub fn apply(&mut self, fill: &Fill) -> Result<&Position, Overflow>;
    pub fn position(&self, trader: u64, instrument: u32) -> Option<&Position>;
    /// Last trade price per instrument (updated by every fill).
    pub fn mark(&self, instrument: u32) -> Option<Ticks>;
    /// Sum over the trader's instruments of pnl(mark); instruments without a mark count 0.
    pub fn trader_pnl(&self, trader: u64) -> Result<Notional, Overflow>;
    pub fn total_pnl(&self) -> Result<Notional, Overflow>;
}

impl RiskEngine {
    pub fn on_fill(&mut self, fill: &Fill) -> Vec<RiskAction>;
    pub fn reset_trader(&mut self, trader: u64); // after an operator unblocks
}
```

Rules:

- **Block a trader** when |net position| in any instrument exceeds
  `max_abs_position`, or when `trader_pnl < -max_trader_loss`. At most once
  per trader until `reset_trader`.
- **Engage the kill switch** when `total_pnl < -kill_switch_loss` or on
  arithmetic overflow. At most once.
- `RiskAction::…{reason}` strings say which limit and which values, for
  example `"net position 10500 > 10000 on instrument 2"`.

## Acceptance criteria

1. Unit tests for each rule, including "exactly once": a second breaching
   fill for an already-blocked trader yields no action.
2. **Property tests (proptest):**
   - PnL of any fill sequence equals an independent recomputation from
     cash flows;
   - positions are additive across fills;
   - offsetting fill pairs are neutral (the existing test);
   - `on_fill` never panics for any `Fill` within the `i64`/`u64` domain.
3. **Overflow:** two fills at `i64::MAX × u64::MAX` produce
   `KillSwitch{engaged: true}`, with no panic.
4. `docs/adr/0016-*.md` exists, is indexed, and is linked from ADR-0005.
5. `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings` and
   `cargo test` pass.

## Files expected to change

- `rust/crates/risk-sentinel/src/{positions.rs,limits.rs,engine.rs}`
- `rust/crates/risk-sentinel/tests/engine_properties.rs` (new, optional)
- `docs/adr/0016-sentinel-overflow-policy.md` (new), `docs/adr/README.md`, `docs/adr/0005-*.md` (one link)

## Out of scope

- gRPC and session handling (task 016). Persisting positions.

## Dependencies

None.
