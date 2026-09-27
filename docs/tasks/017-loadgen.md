# 017: Load generator: concurrency, rate control, scenarios (Rust)

## Goal

Turn `loadgen` from a sequential single-order sender into a tool that can:

- saturate exchange-core with realistic, reproducible order flow;
- drive specific scenarios, such as a risk breach;
- report latency percentiles and outcome counts in human and JSON form.

## Context

- Crate: `rust/crates/lockstep-loadgen` (binary `loadgen`). Today it:
  - sends `--count` identical limit orders sequentially;
  - uses `hdrhistogram` for latency;
  - sets the `x-lockstep-protocol` header;
  - supports `--expect-accepted`.

  `scripts/e2e-smoke.sh` depends on these flags; keep them working.
- Contract: `OrderEntryService` in `proto/lockstep/v1/order_entry.proto`.
  Acks carry `order_id`. Rejections carry `RejectReason`.
  `RESOURCE_EXHAUSTED` means back-pressure (ADR-0006).
- Lints: clippy pedantic with `-D warnings`.

## Interfaces to implement

```
loadgen [--target URL] [--concurrency N] [--rate OPS_PER_SEC | --unlimited]
        [--duration SECS | --count N] [--seed S] [--scenario NAME]
        [--instruments 1,2,3,4] [--traders 1..20] [--json]
        [--expect-accepted]
```

Scenarios:

- **`random`** (default): each trader submits limit orders around a drifting
  mid-price, so a share of them cross and trade. About 20 % are cancels of
  the trader's own accepted orders (tracked from acks) and about 5 % market
  orders. Reproducible from `--seed`, apart from the interleaving between
  concurrent workers.
- **`breach`**: trader 666 keeps buying one instrument aggressively against
  liquidity provided by trader 1, until it receives `TRADER_BLOCKED`
  rejections. It reports the time from the first fill to the first block,
  and exits 0 once blocked (non-zero if never blocked within `--duration`).
  Used by task 019.
- **`single`**: the current behaviour (identical orders), kept for the smoke
  test.

Output:

- per outcome: `accepted`, `rejected` by reason, `resource_exhausted`,
  `errors`;
- latency p50/p90/p99/p99.9/max in µs;
- achieved ops/s.

`--json` prints one JSON object with the same numbers. `serde_json` is an
acceptable new dependency; add it via `[workspace.dependencies]`.

**Back-pressure:** retry `RESOURCE_EXHAUSTED` with jittered back-off, count
the retries, and never treat them as rejections.

## Acceptance criteria

1. **Unit tests:**
   - the rate limiter issues N ± 5 % operations over 2 s at `--rate`;
   - the `random` generator with the same seed produces the same first 1 000
     requests;
   - the cancel generator only targets ids from its own accepted set.
2. `loadgen --scenario single --count 5 --expect-accepted` behaves as before.
   `scripts/e2e-smoke.sh` passes unchanged.
3. Against a local exchange-core, `loadgen --concurrency 8 --duration 5`
   completes and prints the summary. Paste the output in the PR description.
4. `--json` output parses with `jq`.
5. `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings` and
   `cargo test` pass.

## Files expected to change

- `rust/crates/lockstep-loadgen/src/**` (split into `main.rs`, `scenario.rs`, `rate.rs`, `report.rs`)
- `rust/crates/lockstep-loadgen/Cargo.toml`, `rust/Cargo.toml` (workspace deps), `rust/Cargo.lock`

## Out of scope

- Market data subscription in loadgen.
- Distributed load generation.

## Dependencies

None. The `breach` scenario is only *meaningful* after tasks 002, 004, 014 and
016, but it can be implemented and unit-tested now.
