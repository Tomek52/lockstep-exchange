# 22. Execution reports carry a per-command trade index

- **Status:** Proposed
- **Date:** 2026-10-11
- **Amends:** [ADR-0013](0013-risk-feedback-loop.md#adr0013-idempotency-v1) (the sentinel's duplicate-report key) and [task 016](../tasks/016-sentinel-monitor-service.md)

## Context

[ADR-0013](0013-risk-feedback-loop.md) says the sentinel "detects duplicate
execution reports by `(shard_id, shard_sequence)`", and
[task 016](../tasks/016-sentinel-monitor-service.md) turned that into the key
`(shard_id, shard_sequence, is_maker)`, on the stated assumption that one
trade yields a maker and a taker report with the same sequence.

The assumption fails for any command that trades more than once. exchange-core
stamps every event of a command with the command's sequence
(`PublishedEvent{shard(), command.sequence, command.timestamp, event}` in
`ShardRuntime`), so an aggressive order that sweeps N resting orders yields N
`Trade` events, hence 2N reports, with only two distinct keys. The sentinel
applies the first pair and drops the rest as duplicates.

Task 021 reproduced it on a running stack: two resting sells of 10 and one buy
of 20 against `--max-abs-position 15`. The exchange filled trader 2 for 20
lots; the sentinel logged `duplicate execution report ignored` for the second
trade, believed trader 2 held 10, and sent no `BlockTrader`. Positions, PnL and
therefore the position limit and the gross-loss kill switch
([ADR-0021](0021-sentinel-kill-switch-on-gross-loss.md)) are wrong whenever an
order sweeps the book. Tasks 014 to 019 missed it because every test and
scenario filled exactly one resting order per aggressive order.

## <a id="adr0022-trade-index-v1"></a>Decision

**Each `ExecutionReport` carries the zero-based index of its trade among the
trades produced by the command that has its `shard_sequence`. A report is
identified by `(shard_id, shard_sequence, trade_index, is_maker)`.**

- Proto: `uint32 trade_index = 11;` in `lockstep.v1.ExecutionReport`. A sender
  that does not know the index leaves it 0.
- `codec::encode_execution_reports` takes the index as an argument and sets it
  on both reports of the trade.
- `RiskClient::on_events` assigns the index. It keeps, per shard, the sequence
  of the last `Trade` it saw and a counter: the counter is 0 for the first
  trade of a new sequence and increments for each further `Trade` with the same
  sequence. Only `Trade` events count; other events of the command do not
  advance it. The state is per shard because the streams of different shards
  can interleave in the publisher's output, and it is kept across `on_events`
  calls because a command's events can be split across them. It is touched
  only by the publisher thread, which is the only caller of `on_events`.
- The sentinel's dedup key becomes `(shard_id, shard_sequence, trade_index,
  is_maker)`. The bounded window (`DEFAULT_DEDUP_CAPACITY`) is counted in keys
  and now holds up to two keys per trade.
- **The protocol minor version is not bumped.** Minor versions advertise
  features that peers negotiate at runtime
  ([ADR-0006](0006-grpc-service-and-stream-design.md)); nothing is negotiated
  here. The field is optional on the wire, its default (0) reproduces today's
  key, and `buf breaking` accepts it.
- Mixed versions degrade to today's behaviour, not to something worse: an
  older exchange sends 0 for every trade, so a sweep still collides with an
  older or newer sentinel; a newer exchange to an older sentinel is ignored
  by that sentinel (unknown field). Fixing the bug needs both sides upgraded.

## Alternatives considered

- **Stamp an event index into `app::PublishedEvent` in `ShardRuntime` and
  derive the report field from it.** Gives market-data consumers the same
  identity, but touches the egress queue item, the publisher and the replay
  digest, which must not change for a sentinel-only bug. Rejected here; it can
  be added later without changing the wire field.
- **Include the order ids in the key instead of a new field.** A report is then
  identified by `(shard_id, shard_sequence, maker_order, taker_order,
  is_maker)`. Needs no proto change, but couples the sentinel's idempotency to
  the contents of the fill and does not say "this is the nth trade". Two
  trades between the same pair of orders in one command cannot happen today
  (a fully matched maker leaves the book), but the key would silently break if
  that ever changed. Rejected.
- **Give each trade its own sequence number.** The sequence is the journal's
  position of the command; changing it changes the journal and replay.
  Rejected.
- **Stop deduplicating and make the sentinel idempotent another way** (for
  example by resubscribing from a sequence number after a reconnect). A
  larger protocol change than this bug justifies. Rejected for now.

## Consequences

- A sweep is fully accounted for: position limits, PnL and the kill switch see
  every fill.
- Real duplicates after a reconnect are still ignored, because a resent report
  has the same index as the original (the index depends only on the command's
  events, not on the session).
- The `ExecutionReport` message grows by a varint (zero is not encoded).
- Market data still identifies events by `(shard_id, shard_sequence)` only; a
  consumer cannot tell two trades of one command apart except by order. Out of
  scope.
- Enforced by codec and risk-client tests (index assignment, per-shard
  counters, non-trade events), by a sentinel test that applies both reports of
  a sweep and still ignores a replayed sweep, and by a step in
  `scripts/e2e-kill-switch.sh` that fails on the old key.
