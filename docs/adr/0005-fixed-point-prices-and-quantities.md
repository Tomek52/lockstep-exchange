# 5. Fixed-point prices and integer quantities

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

Binary floating point cannot represent most decimal prices exactly (0.1 has no
finite binary expansion). Using doubles for prices leads to:

- price levels that should be equal but compare unequal;
- non-associative sums, where PnL depends on the order of fills;
- platform- and compiler-flag-dependent results (x87 vs SSE, `-ffast-math`,
  FMA contraction), which would break deterministic replay (ADR-0004).

Real exchanges quote prices on a tick grid, so the natural representation is
an integer count of ticks.

## Decision

- **Price** = signed 64-bit integer number of ticks: `domain::Price`, wire
  `sint64 *_ticks`. Signed so that differences and spreads are representable.
  Valid order prices are > 0.
- **Quantity** = unsigned 64-bit integer number of lots: `domain::Quantity`,
  wire `uint64`.
- **Conversion to human units** (tick size, lot size, currency) is reference
  data (`InstrumentSpec`, task 012) and happens only at the edges, for
  display. The matching engine never sees a decimal.
- **Strong types.** `Price`, `Quantity`, `OrderId`, `TraderId`, and the rest
  are distinct types (`StrongInt<Tag, Rep>`). Mixing them does not compile.
  Arithmetic is opt-in per type (`Additive` mixin via deducing `this`), so
  ids cannot be added.
- **Overflow policy.**
  - `InstrumentSpec` bounds every accepted price and quantity. The defaults
    are 1e9 ticks and 1e6 lots, so a single notional is at most 1e15, far
    below `INT64_MAX` (a test in `checked_math_test.cpp` pins this).
  - Where products or unbounded sums occur, use `checked_add/sub/mul`.
  - In Rust (risk-sentinel), notionals are `i128`: any single fill's notional
    fits exactly (|i64| × u64 < 2^127).
- **Protobuf** carries prices as `sint64` (zig-zag), which keeps small
  negative values (spreads) compact.

## Consequences

- Equality and ordering of prices are exact, and `flat_map<Price, Level>`
  keys are stable.
- Replay is bit-exact across compilers and flags.
- Clients must convert decimals to ticks. That is intended: the conversion
  is explicit and the tick grid is enforced.
- **Known limit, found during the skeleton build.** In the sentinel, cash
  accumulated over about two fills of extreme size (i64::MAX × u64::MAX)
  overflows `i128`. Validated ranges make this unreachable. Task 015 decides
  whether to use checked accumulation that treats overflow as a risk breach.
