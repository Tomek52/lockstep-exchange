//! Positions and PnL in fixed-point arithmetic (ADR-0005).
//!
//! Prices arrive as integer ticks and quantities as integer lots, exactly as
//! on the wire. Notionals (price x quantity) are `i128`: the notional of any
//! single fill fits exactly (|i64| x u64 < 2^127) and nothing is ever rounded.
//! Accumulated cash can still overflow after about two fills of extreme size -
//! far outside any validated instrument range; task 015 decides how to treat
//! that (checked accumulation, overflow = risk breach).

/// Price in instrument ticks.
pub type Ticks = i64;
/// Quantity in lots.
pub type Lots = u64;
/// Price x quantity, in tick-lots.
pub type Notional = i128;

/// Side of a fill from the owning trader's point of view.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Side {
    Buy,
    Sell,
}

/// Position of one trader in one instrument.
///
/// Tracked as net quantity plus the cash flow of all fills; mark-to-market PnL
/// is then `cash + net * mark`, which needs no average-price bookkeeping and is
/// exact in integers.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct Position {
    /// Signed net quantity: positive = long.
    pub net_quantity: i128,
    /// Sum of sell notionals minus sum of buy notionals.
    pub cash: Notional,
}

impl Position {
    /// Applies one fill.
    pub fn apply(&mut self, side: Side, price: Ticks, quantity: Lots) {
        let quantity = i128::from(quantity);
        let notional = i128::from(price) * quantity;
        match side {
            Side::Buy => {
                self.net_quantity += quantity;
                self.cash -= notional;
            }
            Side::Sell => {
                self.net_quantity -= quantity;
                self.cash += notional;
            }
        }
    }

    /// Mark-to-market profit and loss at `mark` ticks.
    #[must_use]
    pub fn pnl(&self, mark: Ticks) -> Notional {
        self.cash + self.net_quantity * i128::from(mark)
    }
}

// TODO(task-015): PositionBook keyed by (trader, instrument), last-trade marks,
// per-trader aggregate PnL. See docs/tasks/015-sentinel-positions-pnl.md.

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    #[test]
    fn round_trip_at_higher_price_realises_profit() {
        let mut position = Position::default();
        position.apply(Side::Buy, 100, 10);
        position.apply(Side::Sell, 110, 10);
        assert_eq!(position.net_quantity, 0);
        assert_eq!(position.pnl(0), 100); // flat: the mark is irrelevant
    }

    #[test]
    fn open_position_is_marked_to_market() {
        let mut position = Position::default();
        position.apply(Side::Sell, 50, 4);
        assert_eq!(position.net_quantity, -4);
        assert_eq!(position.pnl(50), 0);
        assert_eq!(position.pnl(45), 20); // short gains when price falls
    }

    #[test]
    fn single_fill_notional_never_overflows() {
        // |i64::MIN| x u64::MAX and i64::MAX x u64::MAX both fit in i128.
        let mut long = Position::default();
        long.apply(Side::Buy, Ticks::MAX, Lots::MAX);
        assert_eq!(long.pnl(Ticks::MAX), 0);

        let mut short = Position::default();
        short.apply(Side::Sell, Ticks::MIN, Lots::MAX);
        assert_eq!(short.pnl(Ticks::MIN), 0);
    }

    proptest! {
        /// Buying and selling the same quantity at the same price is PnL-neutral.
        #[test]
        fn offsetting_fills_are_neutral(price in 1..1_000_000_i64, quantity in 1..1_000_000_u64,
                                        mark in 1..1_000_000_i64) {
            let mut position = Position::default();
            position.apply(Side::Buy, price, quantity);
            position.apply(Side::Sell, price, quantity);
            prop_assert_eq!(position.net_quantity, 0);
            prop_assert_eq!(position.pnl(mark), 0);
        }
    }
}
