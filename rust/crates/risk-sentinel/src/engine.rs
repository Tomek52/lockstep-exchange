//! Pure risk decision logic: fills in, risk actions out. No I/O, no async.

use std::collections::HashSet;

use crate::limits::RiskLimits;
use crate::positions::{Lots, Notional, PositionBook, Side, Ticks};

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

        // TODO(task-016): each fill rescans and sorts every position (traders_in,
        // trader_pnl per holder, total_pnl); keep per-trader indexes and a
        // running total if the Monitor stream makes this the hot path.
        //
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
            Ok(pnl) if loss_exceeds(pnl, self.limits.max_trader_loss) => {
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
            Ok(total) if loss_exceeds(total, self.limits.kill_switch_loss) => self
                .engage_kill_switch(
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

/// `pnl < -limit`, without negating the limit: limits are plain `i128`
/// fields, and negating `i128::MIN` would panic in debug and wrap (silently
/// disabling the limit) in release. A PnL of `i128::MIN` is a loss beyond any
/// limit.
fn loss_exceeds(pnl: Notional, limit: Notional) -> bool {
    pnl.checked_neg().is_none_or(|loss| loss > limit)
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

    /// Limits that never block on position, so only PnL arithmetic can trip.
    fn unbounded_position_limits() -> RiskLimits {
        RiskLimits {
            max_abs_position: i128::MAX,
            ..limits()
        }
    }

    fn kill_switch_reason(actions: &[RiskAction]) -> Option<&str> {
        actions.iter().find_map(|action| match action {
            RiskAction::KillSwitch {
                engaged: true,
                reason,
            } => Some(reason.as_str()),
            _ => None,
        })
    }

    fn blocked_traders(actions: &[RiskAction]) -> Vec<u64> {
        actions
            .iter()
            .filter_map(|action| match action {
                RiskAction::BlockTrader { trader, .. } => Some(*trader),
                RiskAction::KillSwitch { .. } => None,
            })
            .collect()
    }

    #[test]
    fn does_not_block_when_loss_equals_max_trader_loss() {
        // Long 10_000 @ 1_000 marked to 900: loss exactly 1_000_000. Only a
        // loss strictly beyond the limit blocks.
        let mut engine = RiskEngine::new(limits());
        assert!(
            engine
                .on_fill(&fill(1, 2, Side::Buy, 1_000, 10_000))
                .is_empty()
        );
        assert!(engine.on_fill(&fill(2, 2, Side::Sell, 900, 1)).is_empty());
    }

    #[test]
    fn does_not_engage_kill_switch_when_total_loss_equals_limit() {
        // Two traders each long 10_000 @ 1_000, marked to 500: each loses
        // 5_000_000 (within the raised per-trader limit), and the total is
        // exactly -10_000_000, which must not engage the kill switch.
        let mut engine = RiskEngine::new(RiskLimits {
            max_trader_loss: 100_000_000,
            ..limits()
        });
        assert!(
            engine
                .on_fill(&fill(1, 2, Side::Buy, 1_000, 10_000))
                .is_empty()
        );
        assert!(
            engine
                .on_fill(&fill(2, 2, Side::Buy, 1_000, 10_000))
                .is_empty()
        );
        // The seller of the marking lot is short 1 at 500, so it has no PnL.
        let actions = engine.on_fill(&fill(3, 2, Side::Sell, 500, 1));
        assert_eq!(kill_switch_reason(&actions), None);
    }

    #[test]
    fn loss_block_reason_names_limit_and_values() {
        let mut engine = RiskEngine::new(limits());
        let _ = engine.on_fill(&fill(1, 2, Side::Buy, 1_000, 10_000));
        let actions = engine.on_fill(&fill(2, 2, Side::Sell, 899, 1));
        let reason = actions
            .iter()
            .find_map(|action| match action {
                RiskAction::BlockTrader { trader: 1, reason } => Some(reason.as_str()),
                _ => None,
            })
            .expect("trader 1 is blocked");
        assert!(reason.contains("-1010000"), "{reason}");
        assert!(reason.contains("1000000"), "{reason}");
        assert!(reason.contains("loss limit"), "{reason}");
    }

    #[test]
    fn kill_switch_reason_names_limit_and_values() {
        let mut engine = RiskEngine::new(limits());
        let _ = engine.on_fill(&fill(1, 2, Side::Buy, 2_000, 10_000));
        let actions = engine.on_fill(&fill(2, 2, Side::Sell, 999, 1));
        let reason = kill_switch_reason(&actions).expect("kill switch engaged");
        assert!(reason.contains("-10010000"), "{reason}");
        assert!(reason.contains("10000000"), "{reason}");
        assert!(reason.contains("kill switch"), "{reason}");
    }

    #[test]
    fn loss_blocks_trader_at_most_once_until_reset() {
        let mut engine = RiskEngine::new(limits());
        let _ = engine.on_fill(&fill(1, 2, Side::Buy, 1_000, 10_000));
        let first = engine.on_fill(&fill(2, 2, Side::Sell, 899, 1));
        assert_eq!(blocked_traders(&first), vec![1]);

        // The mark falls further: trader 1 is still beyond the loss limit,
        // but is already blocked.
        let second = engine.on_fill(&fill(2, 2, Side::Sell, 800, 1));
        assert!(blocked_traders(&second).is_empty());

        engine.reset_trader(1);
        let third = engine.on_fill(&fill(2, 2, Side::Sell, 800, 1));
        assert_eq!(blocked_traders(&third), vec![1]);
    }

    #[test]
    fn trader_blocked_on_position_is_not_blocked_again_on_loss() {
        // One block per trader, whichever limit tripped first (ADR-0013).
        let mut engine = RiskEngine::new(limits());
        let first = engine.on_fill(&fill(1, 2, Side::Buy, 1_000, 10_001));
        assert_eq!(blocked_traders(&first), vec![1]);
        let second = engine.on_fill(&fill(2, 2, Side::Sell, 800, 1));
        assert!(blocked_traders(&second).is_empty());
    }

    #[test]
    fn one_fill_blocks_every_affected_trader_in_trader_order() {
        // A single marking fill pushes several holders beyond the loss limit;
        // the actions come out in ascending trader id regardless of HashMap
        // iteration order (ADR-0004).
        let mut engine = RiskEngine::new(RiskLimits {
            kill_switch_loss: Notional::MAX,
            ..limits()
        });
        let holders = [42, 7, 1_000, 3, 99, 15, 8, 64];
        for trader in holders {
            assert!(
                engine
                    .on_fill(&fill(trader, 2, Side::Buy, 1_000, 10_000))
                    .is_empty()
            );
        }
        let actions = engine.on_fill(&fill(5, 2, Side::Sell, 800, 1));
        let mut expected = holders.to_vec();
        expected.sort_unstable();
        assert_eq!(blocked_traders(&actions), expected);
    }

    #[test]
    fn overflow_on_buy_side_engages_kill_switch_without_panic() {
        let mut engine = RiskEngine::new(limits());
        let _ = engine.on_fill(&fill(1, 2, Side::Buy, Ticks::MAX, Lots::MAX));
        let actions = engine.on_fill(&fill(1, 2, Side::Buy, Ticks::MAX, Lots::MAX));
        let reason = kill_switch_reason(&actions).expect("kill switch engaged");
        assert!(reason.contains("applying fill for trader 1"), "{reason}");
    }

    #[test]
    fn overflow_engages_kill_switch_at_most_once() {
        let mut engine = RiskEngine::new(limits());
        let _ = engine.on_fill(&fill(1, 2, Side::Sell, Ticks::MAX, Lots::MAX));
        let first = engine.on_fill(&fill(1, 2, Side::Sell, Ticks::MAX, Lots::MAX));
        assert!(kill_switch_reason(&first).is_some());
        let second = engine.on_fill(&fill(1, 2, Side::Sell, Ticks::MAX, Lots::MAX));
        assert!(second.is_empty());
    }

    #[test]
    fn overflow_computing_trader_pnl_engages_kill_switch() {
        // Trader 1 goes long 2 x u64::MAX lots at 1 tick (both fills apply
        // fine), then a single lot trades at i64::MAX: marking that position
        // overflows i128 inside trader 1's PnL.
        let mut engine = RiskEngine::new(unbounded_position_limits());
        assert!(
            engine
                .on_fill(&fill(1, 2, Side::Buy, 1, Lots::MAX))
                .is_empty()
        );
        assert!(
            engine
                .on_fill(&fill(1, 2, Side::Buy, 1, Lots::MAX))
                .is_empty()
        );
        let actions = engine.on_fill(&fill(2, 2, Side::Sell, Ticks::MAX, 1));
        let reason = kill_switch_reason(&actions).expect("kill switch engaged");
        assert!(reason.contains("computing pnl for trader 1"), "{reason}");
    }

    #[test]
    fn overflow_computing_total_pnl_engages_kill_switch() {
        // Traders 1 and 2 each hold u64::MAX lots bought at 1 tick in their own
        // instrument, then each instrument trades at i64::MAX. Each trader's PnL
        // fits in i128; their sum does not.
        let mut engine = RiskEngine::new(unbounded_position_limits());
        assert!(
            engine
                .on_fill(&fill(1, 1, Side::Buy, 1, Lots::MAX))
                .is_empty()
        );
        assert!(
            engine
                .on_fill(&fill(3, 1, Side::Sell, Ticks::MAX, 1))
                .is_empty()
        );
        assert!(
            engine
                .on_fill(&fill(2, 2, Side::Buy, 1, Lots::MAX))
                .is_empty()
        );
        let actions = engine.on_fill(&fill(3, 2, Side::Sell, Ticks::MAX, 1));
        let reason = kill_switch_reason(&actions).expect("kill switch engaged");
        assert!(reason.contains("total pnl"), "{reason}");
    }

    #[test]
    fn extreme_loss_limits_do_not_panic() {
        // Limits are plain i128 fields until task 016 validates configuration;
        // even i128::MIN must not panic or wrap into a disabled limit.
        let mut engine = RiskEngine::new(RiskLimits {
            max_abs_position: i128::MAX,
            max_trader_loss: Notional::MIN,
            kill_switch_loss: Notional::MIN,
        });
        let actions = engine.on_fill(&fill(1, 2, Side::Buy, 100, 1));
        assert_eq!(blocked_traders(&actions), vec![1]);
        assert!(kill_switch_reason(&actions).is_some());
    }
}
