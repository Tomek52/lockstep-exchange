# 021: Execution reports of one command must not collide in the sentinel's dedup

## Goal

Make risk-sentinel account for **every** fill of an order that trades against
several resting orders. Today it applies only the first fill of such a
command and silently drops the others as "duplicates", so its positions, PnL
and limits are wrong exactly when an aggressive order sweeps the book.

## Context

- **Bug, reproduced on 2026-10-11** (found while implementing
  [task 019](019-e2e-kill-switch.md); not covered by it). One shard,
  instrument 2, risk-sentinel with `--max-abs-position 15` and
  `RUST_LOG=debug`:

  ```bash
  L="loadgen --target http://127.0.0.1:15171 --instrument 2 --scenario single --price 100"
  $L --side sell --trader 1 --quantity 10 --count 2 --expect-accepted   # two resting sells of 10
  $L --side buy  --trader 2 --quantity 20 --count 1 --expect-accepted   # one buy of 20: two trades
  ```

  The exchange fills trader 2 for 20 lots (and trader 1 sells 20), so both
  exceed the limit of 15 and the sentinel must send `BlockTrader` for both.
  It sent nothing. Its debug log shows:

  ```text
  duplicate execution report ignored shard_id=0 shard_sequence=5 is_maker=false
  duplicate execution report ignored shard_id=0 shard_sequence=5 is_maker=true
  ```

  Those are the reports of the second trade. The sentinel believes trader 2
  holds 10 lots.
- **Root cause: two components disagree about what a sequence number
  identifies.**
  - exchange-core stamps every event of a command with the command's
    sequence: `PublishedEvent{shard(), command.sequence, command.timestamp,
    event}` (`exchange-core/app/src/shard_runtime.cpp`). A command that
    produces N trades yields N `domain::Trade` events with the same
    `(shard, sequence)`.
  - `codec::encode_execution_reports(event, trade)`
    (`adapters/codec/src/risk_codec.cpp`) copies that into
    `ExecutionReport.shard_sequence` and emits a taker and a maker report
    per trade, told apart only by `is_maker`.
  - The sentinel deduplicates on `(shard_id, shard_sequence, is_maker)`
    (`DedupWindow::first_seen` in `rust/crates/risk-sentinel/src/session.rs`).
    [Task 016](016-sentinel-monitor-service.md) and
    [ADR-0013](../adr/0013-risk-feedback-loop.md#adr0013-idempotency-v1)
    wrote that key down on the assumption that "one trade yields a maker and a
    taker report with the same `(shard_id, shard_sequence)`", i.e. one trade
    per sequence number. That assumption is false for any sweep.
- **Effect.** From the second trade of a command on, both reports are
  dropped; the book is still balanced (both sides of a dropped trade vanish),
  so nothing looks wrong, but positions are understated and the mark ignores
  the later prices. It matters most for the position limit and for the
  gross-loss kill switch
  ([ADR-0021](../adr/0021-sentinel-kill-switch-on-gross-loss.md)). Tasks 014,
  015, 016 and 019 all missed it because every test and scenario fills one
  resting order per aggressive order.
- **Not affected:** `MarketDataService` (task 013). Its events carry
  `shard_sequence` for gap detection, which only needs the sequence to be
  non-decreasing, and the stream preserves the order of events within a
  command. The journal and replay (events are digested in order).
- [ADR-0006](../adr/0006-grpc-service-and-stream-design.md): the contract is
  additive; a wire-breaking change needs `lockstep.v2`, and CI runs
  `buf breaking`. A new field with a new number is compatible. The protocol
  minor version is `0` in both languages (`protocol_minor` in
  `adapters/grpc/include/lockstep/grpc/order_entry_service.hpp`,
  `PROTOCOL_MINOR` in `rust/crates/lockstep-proto/src/lib.rs`).
- **Decision needed:** how a report identifies its trade, and whether that is a
  minor version bump. The deliverable includes **ADR-0022** (status Proposed,
  drafted by the `architect`). The recommendation is below; the ADR may
  choose differently as long as every acceptance criterion holds.

## Interfaces to implement

Recommended design (A): an explicit per-command trade index on the report.

```proto
// proto/lockstep/v1/risk.proto, message ExecutionReport
//
// Zero-based position of this trade among the trades produced by the command
// with this shard_sequence. (shard_id, shard_sequence, trade_index, is_maker)
// identifies a report. A sender that does not know it leaves 0.
uint32 trade_index = 11;
```

```cpp
// adapters/codec/include/lockstep/codec/risk_codec.hpp
[[nodiscard]] std::array<v1::ExecutionReport, 2> encode_execution_reports(
    const app::PublishedEvent& event, const domain::Trade& trade, std::uint32_t trade_index);
```

`RiskClient::on_events` (`adapters/risk_client/src/risk_client.cpp`) computes
the index: one counter per shard (events of one command are contiguous in that
shard's stream, but streams of different shards may interleave in one
`on_events` span), reset when `event.sequence` differs from the previous Trade
of that shard. Only `Trade` events count; other event kinds of the command
(for example `OrderAccepted`) do not advance it.

```rust
// rust/crates/risk-sentinel/src/session.rs
type ReportKey = (u32, u64, u32, bool); // shard_id, shard_sequence, trade_index, is_maker
```

Alternative (B), to be weighed in the ADR: stamp an event index into
`app::PublishedEvent` in `ShardRuntime` and derive the report field from it.
It also benefits market-data consumers, but touches the egress queue item and
the digest, so it is a larger change for no sentinel benefit.

## Acceptance criteria

1. **Codec.** For a trade with a given index, `encode_execution_reports`
   sets `trade_index` on both reports and everything it set before is
   unchanged (`tests/codec/risk_codec_test.cpp`).
2. **Index assignment** (`tests/risk_client`): a command whose taker sweeps
   three makers produces six upstream reports whose keys
   `(shard_id, shard_sequence, trade_index, is_maker)` are pairwise distinct,
   with indices 0, 1, 2 for the three trades. The next command on the same
   shard starts again at 0. Two shards interleaved in one `on_events` call
   keep independent counters. A non-trade event between two trades of the
   same command does not shift the indices.
3. **Sentinel applies every fill** (`tests/monitor_session.rs`): two reports
   that differ only in `trade_index` are both applied. Feeding the sweep from
   the Context (two fills of 10 against a limit of 15) yields `BlockTrader`
   for trader 2.
4. **Real duplicates are still ignored.** The existing
   `reports_replayed_on_a_new_session_are_ignored_and_ids_continue` passes
   unchanged; a replayed *sweep* (all its reports sent twice, as after a
   reconnect) is applied once, not twice. A report without the field (index 0)
   behaves exactly as before.
5. **Dedup window.** The window now holds up to `2 * trades` keys per
   command. A unit test pins that the capacity (`DEFAULT_DEDUP_CAPACITY`,
   `session.rs`) is counted in keys, and the docs say so.
6. **End to end.** `scripts/e2e-kill-switch.sh` gains a step "a sweep is fully
   reported": two resting sells and one buy that takes both, on fresh traders
   and an instrument whose book is otherwise untouched, sized so that **only**
   the sum of both fills exceeds `--max-abs-position`; the script waits for
   `risk command sent.*BlockTrader` for the buyer. This step **fails on
   `main` before the change** (state that in the PR, with the failing output).
7. **Protocol.** `scripts/check-proto.sh` passes (`buf lint`, `buf format`,
   `buf breaking` against `main`). The decision on the protocol minor version
   is recorded in ADR-0022; if the minor is bumped, `protocol_minor` and
   `PROTOCOL_MINOR` agree and the tests that pin the version are updated.
8. **Docs.** ADR-0022 exists, is indexed and in the MkDocs nav, and names
   the [ADR-0013 idempotency rule](../adr/0013-risk-feedback-loop.md#adr0013-idempotency-v1)
   it amends (ADR-0013's own decision text is not edited). The dedup-key
   sentence in [task 016](016-sentinel-monitor-service.md) gets an amendment
   note pointing to ADR-0022. `scripts/docs-check.sh` passes.
9. The repository-wide definition of done in CLAUDE.md (all five presets,
   clang-tidy, format, `cargo fmt`/`clippy -D warnings`/`test`, buf, e2e
   scripts, docs check).

## Files expected to change

- `proto/lockstep/v1/risk.proto`
- `exchange-core/adapters/codec/{include,src}/…/risk_codec.*`
- `exchange-core/adapters/risk_client/src/risk_client.cpp`
- `exchange-core/tests/codec/risk_codec_test.cpp`
- `exchange-core/tests/risk_client/*` (index assignment)
- `rust/crates/risk-sentinel/src/session.rs`,
  `rust/crates/risk-sentinel/tests/monitor_session.rs`
- `scripts/e2e-kill-switch.sh`
- `docs/adr/0022-*.md`, `docs/adr/README.md`, `mkdocs.yml`
- `docs/tasks/016-sentinel-monitor-service.md` (amendment note),
  `docs/tasks/README.md`, `ROADMAP.md`

## Out of scope

- Giving `MarketDataService` events an index within a command.
- Changing how the journal or the digest identify events.
- Making the sentinel's bounded dedup window unbounded or persistent across
  sentinel restarts.
- Reconciling the sentinel's positions against the exchange after a gap.

## Dependencies

- **Hard:** 014 (risk client), 016 (sentinel session and dedup).
- **Related:** 019 (the e2e script this extends), ADR-0021 (the limits whose
  inputs this corrects).
