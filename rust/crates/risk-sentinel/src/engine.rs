//! Pure risk decision logic: fills in, risk actions out. No I/O, no async.

use std::collections::HashSet;

use crate::limits::RiskLimits;
use crate::positions::{Lots, PositionBook, Side, Ticks};

/// One side of an execution, decoded from an `ExecutionReport`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fill {
    pub trader: u64,
    pub instrument: u32,
    pub side: Side,
    pub price: Ticks,
    pub quantity: Lots,
}

/// What the sentinel asks the exchange to do.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RiskAction {
    BlockTrader { trader: u64, reason: String },
    KillSwitch { engaged: bool, reason: String },
}

/// Evaluates every fill against the limits, keeping positions and PnL per
/// trader. Each breach produces its action once (ADR-0013 idempotency): a
/// blocked trader stays blocked until `reset_trader`, and the kill switch
/// engages only once.
#[derive(Debug, Default)]
pub struct RiskEngine {
    limits: RiskLimits,
    book: PositionBook,
    blocked: HashSet<u64>,
    kill_switch_engaged: bool,
}

impl RiskEngine {
    #[must_use]
    pub fn new(limits: RiskLimits) -> Self {
        Self {
            limits,
            book: PositionBook::default(),
            blocked: HashSet::new(),
            kill_switch_engaged: false,
        }
    }

    #[must_use]
    pub fn limits(&self) -> &RiskLimits {
        &self.limits
    }

    /// Applies a fill and returns the actions it triggers (usually none).
    ///
    /// On any accumulation overflow (ADR-0018) the kill switch engages, which
    /// is why this never panics over the whole `i64`/`u64` fill domain.
    pub fn on_fill(&mut self, fill: &Fill) -> Vec<RiskAction> {
        let mut actions = Vec::new();

        // ADR-0018: an overflow applying the fill is a risk breach, mapped to
        // the kill switch rather than a wrap or a panic.
        let Ok(position) = self.book.apply(fill) else {
            self.engage_kill_switch(
                format!(
                    "arithmetic overflow applying fill for trader {}",
                    fill.trader
                ),
                &mut actions,
            );
            return actions;
        };
        let net_quantity = position.net_quantity;

        // Only the fill's own trader can breach the position limit on this fill;
        // their net quantity is what just changed.
        self.check_position(fill.trader, fill.instrument, net_quantity, &mut actions);

        // The fill moved the instrument's mark, so every trader holding it may
        // have a different PnL now, not just the trader who traded. Re-evaluate
        // the loss limit for all of them.
        for trader in self.book.traders_in(fill.instrument) {
            self.check_trader_loss(trader, &mut actions);
        }

        self.check_total(&mut actions);
        actions
    }

    /// Re-arms a trader after an operator unblocks them.
    pub fn reset_trader(&mut self, trader: u64) {
        self.blocked.remove(&trader);
    }

    /// Blocks a trader at most once when their net position in `instrument`
    /// exceeds the absolute limit.
    fn check_position(
        &mut self,
        trader: u64,
        instrument: u32,
        net_quantity: i128,
        actions: &mut Vec<RiskAction>,
    ) {
        if self.blocked.contains(&trader) {
            return;
        }
        let abs_net = net_quantity.unsigned_abs();
        let max_abs = self.limits.max_abs_position.unsigned_abs();
        if abs_net > max_abs {
            self.blocked.insert(trader);
            actions.push(RiskAction::BlockTrader {
                trader,
                // Report the same magnitude used in the comparison, so the
                // message stays consistent even if a negative limit were
                // configured (the documented limit is non-negative).
                reason: format!("net position {abs_net} > {max_abs} on instrument {instrument}"),
            });
        }
    }

    /// Blocks a trader at most once when their aggregate PnL falls below the
    /// loss limit.
    fn check_trader_loss(&mut self, trader: u64, actions: &mut Vec<RiskAction>) {
        if self.blocked.contains(&trader) {
            return;
        }
        match self.book.trader_pnl(trader) {
            Ok(pnl) if pnl < -self.limits.max_trader_loss => {
                self.blocked.insert(trader);
                actions.push(RiskAction::BlockTrader {
                    trader,
                    reason: format!(
                        "trader pnl {pnl} < -{} (loss limit)",
                        self.limits.max_trader_loss
                    ),
                });
            }
            Ok(_) => {}
            // ADR-0018: an overflow computing the trader's PnL engages the kill
            // switch instead of blocking a single trader.
            Err(_) => self.engage_kill_switch(
                format!("arithmetic overflow computing pnl for trader {trader}"),
                actions,
            ),
        }
    }

    /// Engages the kill switch at most once, on aggregate loss or overflow.
    fn check_total(&mut self, actions: &mut Vec<RiskAction>) {
        match self.book.total_pnl() {
            Ok(total) if total < -self.limits.kill_switch_loss => self.engage_kill_switch(
                format!(
                    "total pnl {total} < -{} (kill switch)",
                    self.limits.kill_switch_loss
                ),
                actions,
            ),
            Ok(_) => {}
            Err(_) => self.engage_kill_switch(
                "arithmetic overflow computing total pnl".to_owned(),
                actions,
            ),
        }
    }

    /// Emits a kill switch action once (ADR-0013 idempotency).
    fn engage_kill_switch(&mut self, reason: String, actions: &mut Vec<RiskAction>) {
        if self.kill_switch_engaged {
            return;
        }
        self.kill_switch_engaged = true;
        actions.push(RiskAction::KillSwitch {
            engaged: true,
            reason,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn limits() -> RiskLimits {
        RiskLimits {
            max_abs_position: 10_000,
            max_trader_loss: 1_000_000,
            kill_switch_loss: 10_000_000,
        }
    }

    fn fill(trader: u64, instrument: u32, side: Side, price: Ticks, quantity: Lots) -> Fill {
        Fill {
            trader,
            instrument,
            side,
            price,
            quantity,
        }
    }

    #[test]
    fn blocks_trader_when_net_position_exceeds_max() {
        let mut engine = RiskEngine::new(limits());
        let actions = engine.on_fill(&fill(1, 2, Side::Buy, 100, 10_001));
        assert_eq!(actions.len(), 1);
        match &actions[0] {
            RiskAction::BlockTrader { trader, reason } => {
                assert_eq!(*trader, 1);
                assert!(reason.contains("net position"));
                assert!(reason.contains("10001"));
                assert!(reason.contains("10000"));
                assert!(reason.contains("instrument 2"));
            }
            other @ RiskAction::KillSwitch { .. } => panic!("expected BlockTrader, got {other:?}"),
        }
    }

    #[test]
    fn does_not_block_when_net_position_at_limit() {
        let mut engine = RiskEngine::new(limits());
        // Exactly at the limit is allowed; only strictly greater blocks.
        let actions = engine.on_fill(&fill(1, 2, Side::Buy, 100, 10_000));
        assert!(actions.is_empty());
    }

    #[test]
    fn blocks_trader_on_loss_beyond_max_trader_loss() {
        // Net position stays at the allowed limit (10_000) so only the loss
        // rule can trip. Buy 10_000 @ 1_000, then a trade marks the instrument
        // down to 899: unrealised loss 10_000 * (899 - 1_000) = -1_010_000,
        // which is below -max_trader_loss (-1_000_000).
        let mut engine = RiskEngine::new(limits());
        assert!(
            engine
                .on_fill(&fill(1, 2, Side::Buy, 1_000, 10_000))
                .is_empty()
        );
        let actions = engine.on_fill(&fill(2, 2, Side::Sell, 899, 1));
        assert!(
            actions
                .iter()
                .any(|a| matches!(a, RiskAction::BlockTrader { trader: 1, .. }))
        );
    }

    #[test]
    fn engages_kill_switch_when_total_loss_exceeds_limit() {
        // Net position at the limit (10_000), entry 2_000, marked down to 999:
        // total PnL 10_000 * (999 - 2_000) = -10_010_000 < -kill_switch_loss.
        let mut engine = RiskEngine::new(limits());
        assert!(
            engine
                .on_fill(&fill(1, 2, Side::Buy, 2_000, 10_000))
                .is_empty()
        );
        let actions = engine.on_fill(&fill(2, 2, Side::Sell, 999, 1));
        assert!(
            actions
                .iter()
                .any(|a| matches!(a, RiskAction::KillSwitch { engaged: true, .. }))
        );
    }

    #[test]
    fn blocks_trader_at_most_once_until_reset() {
        let mut engine = RiskEngine::new(limits());
        let first = engine.on_fill(&fill(1, 2, Side::Buy, 100, 10_001));
        assert_eq!(first.len(), 1);

        // A second breaching fill for the already-blocked trader yields nothing.
        let second = engine.on_fill(&fill(1, 2, Side::Buy, 100, 10_001));
        assert!(second.is_empty());

        // After reset, the trader is re-armed and blocks again.
        engine.reset_trader(1);
        let third = engine.on_fill(&fill(1, 2, Side::Buy, 100, 10_001));
        assert_eq!(third.len(), 1);
    }

    #[test]
    fn engages_kill_switch_at_most_once() {
        let mut engine = RiskEngine::new(limits());
        assert!(
            engine
                .on_fill(&fill(1, 2, Side::Buy, 2_000, 10_000))
                .is_empty()
        );
        let first = engine.on_fill(&fill(2, 2, Side::Sell, 999, 1));
        assert!(
            first
                .iter()
                .any(|a| matches!(a, RiskAction::KillSwitch { .. }))
        );

        // Further breaching fills do not re-emit the kill switch (ADR-0013).
        let second = engine.on_fill(&fill(2, 2, Side::Sell, 999, 1));
        assert!(
            !second
                .iter()
                .any(|a| matches!(a, RiskAction::KillSwitch { .. }))
        );
    }

    #[test]
    fn overflow_engages_kill_switch_without_panic() {
        // ADR-0018: accumulated-cash overflow is a risk breach, not a panic.
        let mut engine = RiskEngine::new(limits());
        let _ = engine.on_fill(&fill(1, 2, Side::Sell, Ticks::MAX, Lots::MAX));
        let actions = engine.on_fill(&fill(1, 2, Side::Sell, Ticks::MAX, Lots::MAX));
        assert!(
            actions
                .iter()
                .any(|a| matches!(a, RiskAction::KillSwitch { engaged: true, .. }))
        );
    }
}
