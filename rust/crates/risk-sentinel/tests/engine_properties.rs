//! Property tests for the sentinel's pure decision logic (task 015).
//!
//! These check invariants of positions, PnL and `on_fill` over the whole
//! `i64`/`u64` fill domain, including the overflow corner that ADR-0018 maps
//! to a kill switch rather than a panic.

use proptest::prelude::*;

use risk_sentinel::engine::{Fill, RiskAction, RiskEngine};
use risk_sentinel::positions::{Lots, Notional, PositionBook, Side, Ticks};

/// A side chosen by proptest.
fn any_side() -> impl Strategy<Value = Side> {
    prop_oneof![Just(Side::Buy), Just(Side::Sell)]
}

/// A fill whose fields span the full wire domain (`i64` price, `u64` quantity).
fn any_fill() -> impl Strategy<Value = Fill> {
    (
        any::<u64>(),
        any::<u32>(),
        any_side(),
        any::<Ticks>(),
        any::<Lots>(),
    )
        .prop_map(|(trader, instrument, side, price, quantity)| Fill {
            trader,
            instrument,
            side,
            price,
            quantity,
        })
}

/// A fill that concentrates on a few traders and instruments and favours the
/// extremes of the price and quantity domain, so that accumulation on one
/// position (not only the cross-trader total) reaches the overflow corner.
fn extreme_fill() -> impl Strategy<Value = Fill> {
    let price = prop_oneof![
        Just(Ticks::MAX),
        Just(Ticks::MIN),
        Just(0),
        Just(1),
        Just(-1),
        any::<Ticks>(),
    ];
    let quantity = prop_oneof![Just(Lots::MAX), Just(0), Just(1), any::<Lots>()];
    (0..3_u64, 0..2_u32, any_side(), price, quantity).prop_map(
        |(trader, instrument, side, price, quantity)| Fill {
            trader,
            instrument,
            side,
            price,
            quantity,
        },
    )
}

/// A "small" fill that cannot overflow accumulated cash, for the arithmetic
/// identity properties below.
fn small_fill() -> impl Strategy<Value = Fill> {
    (
        0..4_u64,
        0..3_u32,
        any_side(),
        1..1_000_000_i64,
        1..1_000_000_u64,
    )
        .prop_map(|(trader, instrument, side, price, quantity)| Fill {
            trader,
            instrument,
            side,
            price,
            quantity,
        })
}

/// Independent recomputation of a trader's PnL from raw cash flows, using the
/// last trade price per instrument as the mark.
fn recompute_trader_pnl(fills: &[Fill], trader: u64) -> Notional {
    use std::collections::HashMap;
    let mut net: HashMap<u32, i128> = HashMap::new();
    let mut cash: HashMap<u32, Notional> = HashMap::new();
    let mut marks: HashMap<u32, Ticks> = HashMap::new();
    for fill in fills {
        marks.insert(fill.instrument, fill.price);
        if fill.trader != trader {
            continue;
        }
        let quantity = i128::from(fill.quantity);
        let notional = i128::from(fill.price) * quantity;
        match fill.side {
            Side::Buy => {
                *net.entry(fill.instrument).or_default() += quantity;
                *cash.entry(fill.instrument).or_default() -= notional;
            }
            Side::Sell => {
                *net.entry(fill.instrument).or_default() -= quantity;
                *cash.entry(fill.instrument).or_default() += notional;
            }
        }
    }
    let mut total: Notional = 0;
    for (instrument, net_quantity) in net {
        let mark = i128::from(marks.get(&instrument).copied().unwrap_or(0));
        total += cash.get(&instrument).copied().unwrap_or(0) + net_quantity * mark;
    }
    total
}

proptest! {
    /// (i) The book's PnL for every trader, and in total, equals an
    /// independent recomputation.
    #[test]
    fn pnl_matches_independent_recomputation(fills in prop::collection::vec(small_fill(), 0..32)) {
        let mut book = PositionBook::default();
        for fill in &fills {
            book.apply(fill).expect("small fills never overflow");
        }
        let mut expected_total: Notional = 0;
        for trader in 0..4 {
            let expected = recompute_trader_pnl(&fills, trader);
            prop_assert_eq!(book.trader_pnl(trader).expect("no overflow"), expected);
            expected_total += expected;
        }
        prop_assert_eq!(book.total_pnl().expect("no overflow"), expected_total);
    }

    /// (ii) Net quantity is additive across fills.
    #[test]
    fn positions_are_additive(fills in prop::collection::vec(small_fill(), 0..32)) {
        use std::collections::HashMap;
        let mut book = PositionBook::default();
        let mut expected_net: HashMap<(u64, u32), i128> = HashMap::new();
        for fill in &fills {
            book.apply(fill).expect("small fills never overflow");
            let delta = i128::from(fill.quantity);
            let signed = match fill.side {
                Side::Buy => delta,
                Side::Sell => -delta,
            };
            *expected_net.entry((fill.trader, fill.instrument)).or_default() += signed;
        }
        for ((trader, instrument), net) in expected_net {
            let actual = book.position(trader, instrument).map_or(0, |p| p.net_quantity);
            prop_assert_eq!(actual, net);
        }
    }

    /// (iii) A buy and an offsetting sell at the same price are PnL-neutral.
    #[test]
    fn offsetting_fill_pairs_are_neutral(
        trader in any::<u64>(),
        instrument in any::<u32>(),
        price in 1..1_000_000_i64,
        quantity in 1..1_000_000_u64,
    ) {
        let mut book = PositionBook::default();
        book.apply(&Fill { trader, instrument, side: Side::Buy, price, quantity })
            .expect("no overflow");
        book.apply(&Fill { trader, instrument, side: Side::Sell, price, quantity })
            .expect("no overflow");
        prop_assert_eq!(book.position(trader, instrument).map_or(0, |p| p.net_quantity), 0);
        prop_assert_eq!(book.trader_pnl(trader).expect("no overflow"), 0);
    }

    /// (iv) `on_fill` never panics for any fill in the wire domain (ADR-0018:
    /// overflow degrades into a kill switch, never a panic or a wrap).
    #[test]
    fn on_fill_never_panics(
        fills in prop::collection::vec(prop_oneof![any_fill(), extreme_fill()], 0..16),
    ) {
        let mut engine = RiskEngine::default();
        for fill in &fills {
            let actions = engine.on_fill(fill);
            // Any action must carry a non-empty reason string.
            for action in &actions {
                match action {
                    RiskAction::BlockTrader { reason, .. }
                    | RiskAction::KillSwitch { reason, .. } => prop_assert!(!reason.is_empty()),
                }
            }
        }
    }

    /// (v) `on_fill` is a pure function of the fill sequence: two engines (each
    /// with its own `HashMap` hashing seed) emit identical actions (ADR-0004).
    #[test]
    fn on_fill_actions_do_not_depend_on_hash_order(
        fills in prop::collection::vec(prop_oneof![small_fill(), extreme_fill()], 0..32),
    ) {
        let mut first = RiskEngine::default();
        let mut second = RiskEngine::default();
        for fill in &fills {
            prop_assert_eq!(first.on_fill(fill), second.on_fill(fill));
        }
    }
}
