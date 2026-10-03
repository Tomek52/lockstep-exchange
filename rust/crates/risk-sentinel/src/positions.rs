//! Positions and PnL in fixed-point arithmetic (ADR-0005).
//!
//! Prices arrive as integer ticks and quantities as integer lots, exactly as
//! on the wire. Notionals (price x quantity) are `i128`: the notional of any
//! single fill fits exactly (|i64| x u64 < 2^127) and nothing is ever rounded.
//! Accumulated cash can still overflow after about two fills of extreme size -
//! far outside any validated instrument range. ADR-0018 fixes the policy:
//! checked accumulation, and an overflow is a risk breach, never a wrap or a
//! panic.

use std::collections::HashMap;

use crate::engine::Fill;

/// Price in instrument ticks.
pub type Ticks = i64;
/// Quantity in lots.
pub type Lots = u64;
/// Price x quantity, in tick-lots.
pub type Notional = i128;

/// Accumulated-cash or PnL arithmetic exceeded `i128` (ADR-0018).
///
/// A plain marker: there is nothing a caller can do to recover a correct
/// number, only to react safely (engage the kill switch).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Overflow;

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
    /// Applies one fill with checked accumulation.
    ///
    /// A single fill's notional always fits (|i64| x u64 < 2^127), but the
    /// running `cash` and `net_quantity` can overflow after extreme fills;
    /// ADR-0018 treats that overflow as a risk breach rather than wrapping or
    /// panicking.
    pub fn apply(&mut self, side: Side, price: Ticks, quantity: Lots) -> Result<(), Overflow> {
        let quantity = i128::from(quantity);
        // Single-fill notional cannot overflow i128, so a plain product is safe
        // here; only the accumulation below is checked (ADR-0018).
        let notional = i128::from(price) * quantity;
        let (net, cash) = match side {
            Side::Buy => (
                self.net_quantity.checked_add(quantity).ok_or(Overflow)?,
                self.cash.checked_sub(notional).ok_or(Overflow)?,
            ),
            Side::Sell => (
                self.net_quantity.checked_sub(quantity).ok_or(Overflow)?,
                self.cash.checked_add(notional).ok_or(Overflow)?,
            ),
        };
        self.net_quantity = net;
        self.cash = cash;
        Ok(())
    }

    /// Mark-to-market profit and loss at `mark` ticks.
    #[must_use = "an Overflow result must engage the kill switch (ADR-0018)"]
    pub fn pnl(&self, mark: Ticks) -> Result<Notional, Overflow> {
        self.net_quantity
            .checked_mul(i128::from(mark))
            .and_then(|marked| self.cash.checked_add(marked))
            .ok_or(Overflow)
    }
}

/// Positions of every trader in every instrument, with the last trade price per
/// instrument as its mark.
#[derive(Debug, Default)]
pub struct PositionBook {
    positions: HashMap<(u64, u32), Position>,
    marks: HashMap<u32, Ticks>,
}

impl PositionBook {
    /// Applies one fill: updates the `(trader, instrument)` position and sets
    /// the instrument's mark to the fill price.
    ///
    /// Returns `Overflow` if accumulation exceeds `i128` (ADR-0018); the
    /// position is left unchanged in that case so the book stays consistent.
    pub fn apply(&mut self, fill: &Fill) -> Result<&Position, Overflow> {
        let position = self
            .positions
            .entry((fill.trader, fill.instrument))
            .or_default();
        let mut candidate = *position;
        candidate.apply(fill.side, fill.price, fill.quantity)?;
        *position = candidate;
        self.marks.insert(fill.instrument, fill.price);
        Ok(position)
    }

    /// The position of one trader in one instrument, if any fill touched it.
    #[must_use]
    pub fn position(&self, trader: u64, instrument: u32) -> Option<&Position> {
        self.positions.get(&(trader, instrument))
    }

    /// Last trade price per instrument (updated by every fill).
    #[must_use]
    pub fn mark(&self, instrument: u32) -> Option<Ticks> {
        self.marks.get(&instrument).copied()
    }

    /// The distinct traders holding a position in `instrument`, sorted so the
    /// caller's output does not depend on `HashMap` iteration order.
    #[must_use]
    pub fn traders_in(&self, instrument: u32) -> Vec<u64> {
        let mut traders: Vec<u64> = self
            .positions
            .keys()
            .filter(|&&(_, i)| i == instrument)
            .map(|&(trader, _)| trader)
            .collect();
        traders.sort_unstable();
        traders
    }

    /// Sum over the trader's instruments of `pnl(mark)`; instruments without a
    /// mark count 0.
    #[must_use = "an Overflow result must engage the kill switch (ADR-0018)"]
    pub fn trader_pnl(&self, trader: u64) -> Result<Notional, Overflow> {
        // Sum over sorted keys so the fold order, and therefore any Overflow
        // decision near the i128 boundary, does not depend on HashMap iteration
        // order (ADR-0004 determinism; mirrors traders_in).
        let mut keys: Vec<(u64, u32)> = self
            .positions
            .keys()
            .filter(|&&(fill_trader, _)| fill_trader == trader)
            .copied()
            .collect();
        keys.sort_unstable();
        let mut total: Notional = 0;
        for key in keys {
            let (_, instrument) = key;
            let position = &self.positions[&key];
            let mark = self.marks.get(&instrument).copied().unwrap_or(0);
            total = total.checked_add(position.pnl(mark)?).ok_or(Overflow)?;
        }
        Ok(total)
    }

    /// Aggregate PnL across every trader and instrument.
    #[must_use = "an Overflow result must engage the kill switch (ADR-0018)"]
    pub fn total_pnl(&self) -> Result<Notional, Overflow> {
        // Sum over sorted keys so the fold order, and therefore any Overflow
        // decision near the i128 boundary, does not depend on HashMap iteration
        // order (ADR-0004 determinism; mirrors traders_in).
        let mut keys: Vec<(u64, u32)> = self.positions.keys().copied().collect();
        keys.sort_unstable();
        let mut total: Notional = 0;
        for key in keys {
            let (_, instrument) = key;
            let position = &self.positions[&key];
            let mark = self.marks.get(&instrument).copied().unwrap_or(0);
            total = total.checked_add(position.pnl(mark)?).ok_or(Overflow)?;
        }
        Ok(total)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::Fill;
    use proptest::prelude::*;

    #[test]
    fn round_trip_at_higher_price_realises_profit() {
        let mut position = Position::default();
        position.apply(Side::Buy, 100, 10).unwrap();
        position.apply(Side::Sell, 110, 10).unwrap();
        assert_eq!(position.net_quantity, 0);
        assert_eq!(position.pnl(0).unwrap(), 100); // flat: the mark is irrelevant
    }

    #[test]
    fn open_position_is_marked_to_market() {
        let mut position = Position::default();
        position.apply(Side::Sell, 50, 4).unwrap();
        assert_eq!(position.net_quantity, -4);
        assert_eq!(position.pnl(50).unwrap(), 0);
        assert_eq!(position.pnl(45).unwrap(), 20); // short gains when price falls
    }

    #[test]
    fn single_fill_notional_never_overflows() {
        // |i64::MIN| x u64::MAX and i64::MAX x u64::MAX both fit in i128.
        let mut long = Position::default();
        long.apply(Side::Buy, Ticks::MAX, Lots::MAX).unwrap();
        assert_eq!(long.pnl(Ticks::MAX).unwrap(), 0);

        let mut short = Position::default();
        short.apply(Side::Sell, Ticks::MIN, Lots::MAX).unwrap();
        assert_eq!(short.pnl(Ticks::MIN).unwrap(), 0);
    }

    proptest! {
        /// Buying and selling the same quantity at the same price is PnL-neutral.
        #[test]
        fn offsetting_fills_are_neutral(price in 1..1_000_000_i64, quantity in 1..1_000_000_u64,
                                        mark in 1..1_000_000_i64) {
            let mut position = Position::default();
            position.apply(Side::Buy, price, quantity).expect("small fill never overflows");
            position.apply(Side::Sell, price, quantity).expect("small fill never overflows");
            prop_assert_eq!(position.net_quantity, 0);
            prop_assert_eq!(position.pnl(mark).expect("small position never overflows"), 0);
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
    fn book_tracks_positions_per_trader_and_instrument() {
        let mut book = PositionBook::default();
        book.apply(&fill(1, 7, Side::Buy, 100, 10)).unwrap();
        book.apply(&fill(1, 8, Side::Sell, 50, 4)).unwrap();
        book.apply(&fill(2, 7, Side::Buy, 100, 3)).unwrap();

        assert_eq!(book.position(1, 7).unwrap().net_quantity, 10);
        assert_eq!(book.position(1, 8).unwrap().net_quantity, -4);
        assert_eq!(book.position(2, 7).unwrap().net_quantity, 3);
        assert!(book.position(2, 8).is_none());
    }

    #[test]
    fn book_mark_is_last_trade_price() {
        let mut book = PositionBook::default();
        book.apply(&fill(1, 7, Side::Buy, 100, 10)).unwrap();
        book.apply(&fill(2, 7, Side::Sell, 123, 1)).unwrap();
        assert_eq!(book.mark(7), Some(123));
        assert_eq!(book.mark(99), None);
    }

    #[test]
    fn trader_pnl_sums_instruments_marked_to_last_trade() {
        let mut book = PositionBook::default();
        // Long 10 @ 100 on instrument 7, mark then moves to 110 via another fill.
        book.apply(&fill(1, 7, Side::Buy, 100, 10)).unwrap();
        book.apply(&fill(2, 7, Side::Sell, 110, 1)).unwrap();
        // Trader 1 is long 10 at mark 110: unrealised profit 10 * (110 - 100) = 100.
        assert_eq!(book.trader_pnl(1).unwrap(), 100);
    }

    #[test]
    fn total_pnl_sums_all_traders() {
        let mut book = PositionBook::default();
        book.apply(&fill(1, 7, Side::Buy, 100, 10)).unwrap();
        book.apply(&fill(2, 7, Side::Sell, 100, 10)).unwrap();
        // Both marked at 100: trader 1 flat-value long, trader 2 flat-value short,
        // their PnLs cancel to zero.
        assert_eq!(book.total_pnl().unwrap(), 0);
    }

    #[test]
    fn accumulated_cash_overflow_is_reported() {
        // ADR-0018: two fills at the extreme corner overflow accumulated cash.
        let mut book = PositionBook::default();
        book.apply(&fill(1, 7, Side::Sell, Ticks::MAX, Lots::MAX))
            .unwrap();
        assert_eq!(
            book.apply(&fill(1, 7, Side::Sell, Ticks::MAX, Lots::MAX)),
            Err(Overflow)
        );
    }

    #[test]
    fn pnl_sums_do_not_depend_on_hash_order() {
        // Trader 1: short u64::MAX lots on instrument 0, long u64::MAX lots on
        // instruments 1 and 2, all bought or sold at 1 tick, then every
        // instrument trades at i64::MAX. The three PnLs are -y, +y, +y: the
        // final sum fits in i128, but adding the two +y first does not. The
        // fold must use a fixed order, never HashMap order, so every fresh book
        // (each with its own hashing seed) must agree (ADR-0004).
        let y = i128::from(Lots::MAX) * (i128::from(Ticks::MAX) - 1);
        for _ in 0..20 {
            let mut book = PositionBook::default();
            book.apply(&fill(1, 0, Side::Sell, 1, Lots::MAX)).unwrap();
            book.apply(&fill(1, 1, Side::Buy, 1, Lots::MAX)).unwrap();
            book.apply(&fill(1, 2, Side::Buy, 1, Lots::MAX)).unwrap();
            for instrument in 0..3 {
                book.apply(&fill(2, instrument, Side::Sell, Ticks::MAX, 1))
                    .unwrap();
            }
            assert_eq!(book.trader_pnl(1), Ok(y));
            assert_eq!(book.total_pnl(), Ok(y));
        }
    }
}
