//! Order-flow generators. They are pure state machines: no I/O, no clock, so
//! the same seed always yields the same request sequence (given the same
//! sequence of outcomes).

use lockstep_proto::v1::{
    CancelOrderRequest, OrderType, RejectReason, Side, SubmitOrderRequest, TimeInForce,
};

/// Deterministic `SplitMix64` generator. Small, fast, and good enough for load
/// shaping; not for anything security related.
#[derive(Debug, Clone)]
pub(crate) struct Rng(u64);

impl Rng {
    pub(crate) fn new(seed: u64) -> Self {
        Self(seed)
    }

    /// Derives an independent stream for worker `index` from a base seed.
    pub(crate) fn for_worker(seed: u64, index: u64) -> Self {
        let mut mixer = Self::new(seed ^ index.wrapping_mul(0x9E37_79B9_7F4A_7C15));
        mixer.next_u64();
        Self::new(mixer.next_u64())
    }

    pub(crate) fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform in `0..n`. `n` must be non-zero.
    pub(crate) fn below(&mut self, n: u64) -> u64 {
        debug_assert!(n > 0);
        // Modulo bias is negligible for the small `n` used here.
        self.next_u64() % n
    }

    /// True with probability `percent` / 100.
    pub(crate) fn chance(&mut self, percent: u64) -> bool {
        self.below(100) < percent
    }
}

/// One request to send.
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum Action {
    Submit(SubmitOrderRequest),
    Cancel(CancelOrderRequest),
}

/// What the exchange answered, as far as a generator cares.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Outcome {
    Accepted {
        order_id: u64,
    },
    Rejected(RejectReason),
    /// Gave up after back-pressure retries, or a transport error.
    Failed,
}

/// Client order ids are unique per trader. Workers may share a trader, so each
/// worker owns a disjoint id range: worker `w` uses `(w << 40) + n`.
const WORKER_ID_SHIFT: u32 = 40;

fn client_order_id(worker: u64, n: u64) -> u64 {
    (worker << WORKER_ID_SHIFT) + n
}

// ---------------------------------------------------------------- single

/// The original behaviour: identical orders, forever.
#[derive(Debug)]
pub(crate) struct Single {
    request: SubmitOrderRequest,
    worker: u64,
    issued: u64,
}

impl Single {
    pub(crate) fn new(worker: u64, request: SubmitOrderRequest) -> Self {
        Self {
            request,
            worker,
            issued: 0,
        }
    }

    pub(crate) fn next(&mut self) -> Action {
        self.issued += 1;
        let mut request = self.request;
        request.client_order_id = client_order_id(self.worker, self.issued);
        Action::Submit(request)
    }
}

// ---------------------------------------------------------------- random

const MID_ANCHOR: i64 = 1_000;
/// The mid price is pulled back when it strays this far from the anchor, so
/// independent workers keep trading around the same level and orders cross.
const MID_BAND: i64 = 15;
const PRICE_SPREAD: i64 = 5;
/// Offsets are drawn from `-PRICE_SPREAD..=PRICE_SPREAD`, shifted by `SKEW`.
const RANGE_WIDTH: u64 = 2 * PRICE_SPREAD as u64 + 1;
const SKEW: i64 = 5;
const CANCEL_PERCENT: u64 = 20;
const MARKET_PERCENT: u64 = 5;
const IOC_PERCENT: u64 = 10;
const MAX_LIMIT_QUANTITY: u64 = 100;
const MAX_MARKET_QUANTITY: u64 = 20;

/// A resting order this generator may cancel later.
#[derive(Debug, Clone, Copy)]
struct Resting {
    instrument_id: u32,
    order_id: u64,
}

/// Limit orders around a drifting mid, some crossing, plus cancels of the
/// generator's own accepted orders and a few market orders.
#[derive(Debug)]
pub(crate) struct RandomFlow {
    rng: Rng,
    trader_id: u64,
    worker: u64,
    instruments: Vec<u32>,
    mid: i64,
    issued: u64,
    /// Accepted submits that have not been cancelled by us yet.
    accepted: Vec<Resting>,
    /// The submit awaiting its outcome (instrument and kind).
    pending: Option<Pending>,
}

#[derive(Debug, Clone, Copy)]
enum Pending {
    Submit { instrument_id: u32, rests: bool },
    Cancel,
}

impl RandomFlow {
    pub(crate) fn new(seed: u64, worker: u64, trader_id: u64, instruments: Vec<u32>) -> Self {
        assert!(
            !instruments.is_empty(),
            "at least one instrument is required"
        );
        Self {
            rng: Rng::for_worker(seed, worker),
            trader_id,
            worker,
            instruments,
            mid: MID_ANCHOR,
            issued: 0,
            accepted: Vec::new(),
            pending: None,
        }
    }

    fn drift(&mut self) {
        let mut step = i64::try_from(self.rng.below(3)).unwrap_or(1) - 1;
        if self.mid > MID_ANCHOR + MID_BAND {
            step = -1;
        } else if self.mid < MID_ANCHOR - MID_BAND {
            step = 1;
        }
        self.mid += step;
    }

    pub(crate) fn next(&mut self) -> Action {
        self.drift();
        if !self.accepted.is_empty() && self.rng.chance(CANCEL_PERCENT) {
            let index = usize::try_from(self.rng.below(self.accepted.len() as u64)).unwrap_or(0);
            let target = self.accepted.swap_remove(index);
            self.pending = Some(Pending::Cancel);
            return Action::Cancel(CancelOrderRequest {
                trader_id: self.trader_id,
                instrument_id: target.instrument_id,
                order_id: target.order_id,
            });
        }

        self.issued += 1;
        let instrument_id = self.instruments
            [usize::try_from(self.rng.below(self.instruments.len() as u64)).unwrap_or(0)];
        let side = if self.rng.chance(50) {
            Side::Buy
        } else {
            Side::Sell
        };
        let market = self.rng.chance(MARKET_PERCENT);
        let (order_type, time_in_force, price_ticks, quantity) = if market {
            (
                OrderType::Market,
                TimeInForce::Ioc,
                0,
                1 + self.rng.below(MAX_MARKET_QUANTITY),
            )
        } else {
            // Buys sit a little below the mid and sells a little above, so
            // most orders rest and only the overlap of the ranges trades.
            let raw = i64::try_from(self.rng.below(RANGE_WIDTH)).unwrap_or(0);
            let offset = if side == Side::Buy {
                raw - PRICE_SPREAD - SKEW
            } else {
                raw - PRICE_SPREAD + SKEW
            };
            let tif = if self.rng.chance(IOC_PERCENT) {
                TimeInForce::Ioc
            } else {
                TimeInForce::Gtc
            };
            (
                OrderType::Limit,
                tif,
                self.mid + offset,
                1 + self.rng.below(MAX_LIMIT_QUANTITY),
            )
        };
        self.pending = Some(Pending::Submit {
            instrument_id,
            rests: !market && time_in_force == TimeInForce::Gtc,
        });
        Action::Submit(SubmitOrderRequest {
            trader_id: self.trader_id,
            client_order_id: client_order_id(self.worker, self.issued),
            instrument_id,
            side: side.into(),
            r#type: order_type.into(),
            time_in_force: time_in_force.into(),
            price_ticks,
            quantity,
        })
    }

    /// Reports the outcome of the action returned by the last [`next`](Self::next).
    pub(crate) fn observe(&mut self, outcome: Outcome) {
        if let (
            Some(Pending::Submit {
                instrument_id,
                rests: true,
            }),
            Outcome::Accepted { order_id },
        ) = (self.pending.take(), outcome)
        {
            self.accepted.push(Resting {
                instrument_id,
                order_id,
            });
        }
    }
}

// ---------------------------------------------------------------- breach

pub(crate) const BREACH_TRADER: u64 = 666;
pub(crate) const LIQUIDITY_TRADER: u64 = 1;
const BREACH_PRICE: i64 = 1_000;
const BREACH_QUANTITY: u64 = 100;

/// Trader 666 buys aggressively against liquidity that trader 1 provides.
/// Each round trader 1 offers `BREACH_QUANTITY` at `BREACH_PRICE`, then trader
/// 666 buys exactly that, so the buy always has something to trade against.
#[derive(Debug)]
pub(crate) struct Breach {
    instrument_id: u32,
    round: u64,
    next_is_buy: bool,
}

impl Breach {
    pub(crate) fn new(instrument_id: u32) -> Self {
        Self {
            instrument_id,
            round: 0,
            next_is_buy: false,
        }
    }

    /// True when the action returned by the last call is the aggressive buy.
    pub(crate) fn last_was_buy(&self) -> bool {
        !self.next_is_buy
    }

    pub(crate) fn next(&mut self) -> Action {
        let buy = self.next_is_buy;
        if !buy {
            self.round += 1;
        }
        self.next_is_buy = !buy;
        // Distinct id spaces for the two traders are unnecessary (ids are per
        // trader), but the round number keeps every id unique.
        Action::Submit(SubmitOrderRequest {
            trader_id: if buy { BREACH_TRADER } else { LIQUIDITY_TRADER },
            client_order_id: self.round,
            instrument_id: self.instrument_id,
            side: if buy { Side::Buy } else { Side::Sell }.into(),
            r#type: OrderType::Limit.into(),
            time_in_force: TimeInForce::Gtc.into(),
            price_ticks: BREACH_PRICE,
            quantity: BREACH_QUANTITY,
        })
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashSet;

    use super::*;

    /// Plays the exchange: accepts everything with increasing order ids.
    fn run(flow: &mut RandomFlow, n: usize) -> Vec<Action> {
        let mut next_id = 1;
        (0..n)
            .map(|_| {
                let action = flow.next();
                flow.observe(Outcome::Accepted { order_id: next_id });
                next_id += 1;
                action
            })
            .collect()
    }

    #[test]
    fn same_seed_gives_the_same_first_thousand_requests() {
        let make = |seed| RandomFlow::new(seed, 0, 1, vec![1, 2, 3, 4]);
        let a = run(&mut make(42), 1_000);
        let b = run(&mut make(42), 1_000);
        let c = run(&mut make(43), 1_000);
        assert_eq!(a, b);
        assert_ne!(a, c);
    }

    #[test]
    fn workers_with_the_same_seed_draw_different_streams() {
        let a = run(&mut RandomFlow::new(7, 0, 1, vec![1]), 50);
        let b = run(&mut RandomFlow::new(7, 1, 1, vec![1]), 50);
        assert_ne!(a, b);
    }

    #[test]
    fn cancels_only_target_ids_this_flow_had_accepted_and_each_only_once() {
        let mut flow = RandomFlow::new(9, 0, 5, vec![1, 2]);
        let mut accepted: HashSet<(u32, u64)> = HashSet::new();
        let mut cancelled: HashSet<(u32, u64)> = HashSet::new();
        let mut next_id = 100;
        let mut cancels = 0;
        for _ in 0..2_000 {
            match flow.next() {
                Action::Submit(request) => {
                    assert_eq!(request.trader_id, 5);
                    // Reject every fifth so rejected ids must never be cancelled.
                    if next_id % 5 == 0 {
                        flow.observe(Outcome::Rejected(RejectReason::TraderBlocked));
                    } else {
                        if request.time_in_force == TimeInForce::Gtc as i32
                            && request.r#type == OrderType::Limit as i32
                        {
                            accepted.insert((request.instrument_id, next_id));
                        }
                        flow.observe(Outcome::Accepted { order_id: next_id });
                    }
                    next_id += 1;
                }
                Action::Cancel(request) => {
                    cancels += 1;
                    assert_eq!(request.trader_id, 5);
                    let key = (request.instrument_id, request.order_id);
                    assert!(accepted.contains(&key), "cancel of unknown order {key:?}");
                    assert!(cancelled.insert(key), "order {key:?} cancelled twice");
                    flow.observe(Outcome::Accepted {
                        order_id: request.order_id,
                    });
                }
            }
        }
        assert!(cancels > 0, "the mix should include cancels");
    }

    #[test]
    fn mix_has_roughly_the_documented_shares_and_some_orders_cross() {
        let mut flow = RandomFlow::new(1, 0, 1, vec![1]);
        let actions = run(&mut flow, 10_000);
        let cancels = actions
            .iter()
            .filter(|a| matches!(a, Action::Cancel(_)))
            .count();
        let markets = actions
            .iter()
            .filter(|a| matches!(a, Action::Submit(r) if r.r#type == OrderType::Market as i32))
            .count();
        assert!((1_500..2_500).contains(&cancels), "cancels={cancels}");
        assert!((200..800).contains(&markets), "markets={markets}");

        // Crossing: some buy limit priced at or above some sell limit.
        let (mut best_buy, mut best_sell) = (i64::MIN, i64::MAX);
        for action in &actions {
            if let Action::Submit(r) = action
                && r.r#type == OrderType::Limit as i32
            {
                if r.side == Side::Buy as i32 {
                    best_buy = best_buy.max(r.price_ticks);
                } else {
                    best_sell = best_sell.min(r.price_ticks);
                }
            }
        }
        assert!(best_buy >= best_sell);
    }

    #[test]
    fn requests_are_valid_per_the_contract() {
        let mut flow = RandomFlow::new(3, 2, 1, vec![1, 2]);
        for action in run(&mut flow, 2_000) {
            if let Action::Submit(r) = action {
                assert!(r.quantity > 0);
                if r.r#type == OrderType::Market as i32 {
                    assert_eq!(r.price_ticks, 0);
                } else {
                    assert!(r.price_ticks > 0);
                }
                assert_eq!(r.client_order_id >> WORKER_ID_SHIFT, 2);
            }
        }
    }

    #[test]
    fn single_repeats_one_order_with_increasing_client_ids() {
        let template = SubmitOrderRequest {
            trader_id: 1,
            instrument_id: 2,
            price_ticks: 100,
            quantity: 10,
            ..Default::default()
        };
        let mut single = Single::new(0, template);
        let ids: Vec<u64> = (0..3)
            .map(|_| match single.next() {
                Action::Submit(r) => r.client_order_id,
                Action::Cancel(_) => unreachable!(),
            })
            .collect();
        assert_eq!(ids, [1, 2, 3]);
    }

    #[test]
    fn breach_alternates_liquidity_then_aggressive_buy() {
        let mut breach = Breach::new(3);
        let first = breach.next();
        assert!(!breach.last_was_buy());
        let second = breach.next();
        assert!(breach.last_was_buy());
        let (Action::Submit(sell), Action::Submit(buy)) = (first, second) else {
            panic!("breach only submits");
        };
        assert_eq!(
            (sell.trader_id, sell.side),
            (LIQUIDITY_TRADER, Side::Sell as i32)
        );
        assert_eq!((buy.trader_id, buy.side), (BREACH_TRADER, Side::Buy as i32));
        assert_eq!(sell.price_ticks, buy.price_ticks);
        assert_eq!(sell.quantity, buy.quantity);
        assert_eq!(buy.instrument_id, 3);
    }
}
