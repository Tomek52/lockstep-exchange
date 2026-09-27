# 014: Risk client: execution reports, ack aggregation, reconnect

## Goal

Complete exchange-core's side of the risk loop. Specifically:

- forward every fill to risk-sentinel as execution reports;
- acknowledge risk commands once *all* shards applied them;
- send heartbeats;
- reconnect with back-off, with link transitions journaled.

## Context

- [ADR-0013](../adr/0013-risk-feedback-loop.md) defines the semantics. Read it
  first.
- Contract: `RiskSentinelService.Monitor` in `proto/lockstep/v1/risk.proto`
  (`MonitorRequest`/`MonitorResponse` envelopes, `ExecutionReport`,
  `CommandApplied`).
- The current adapter
  (`exchange-core/adapters/risk_client/src/risk_client.cpp`) does:
  - the handshake;
  - decoding of inbound `RiskCommand`s and `CommandIngress::broadcast`;
  - `RiskLinkStatus` broadcast on accept/close.

  `on_events()` is empty (`TODO(task-014)`), `OnWriteDone` is a stub, and
  there is no reconnect.
- `RiskClient` is an `app::EventSubscriber`. `on_events` runs on the
  publisher thread and must not block.
- The shard count is known from `RiskClientConfig::shard_count`. Each shard
  emits one `RiskCommandApplied{id}` per risk command.
- A `Trade` event has maker and taker trader/order ids, and
  `PublishedEvent` carries `(shard, sequence, timestamp)`.

## Interfaces to implement

- **Execution reports:** each `Trade` produces two `ExecutionReport`s:
  - taker: `side = aggressor_side`, `is_maker = false`;
  - maker: `side = opposite`, `is_maker = true`.

  Both carry the event's shard id, shard sequence and timestamp. Put the
  encoding in `adapters/codec` (`risk_codec.hpp`) with its own tests.
- **Upstream write queue:** `on_events` appends encoded messages to a
  mutex-protected deque (adapter-level; locks are fine outside the domain and
  runtime). The reactor keeps exactly one `StartWrite` outstanding and pulls
  the next message in `OnWriteDone`. The queue is bounded (for example
  100 000). On overflow, log an error and drop the session, which leads to a
  reconnect. Never block the publisher.
- **Ack aggregation:** `std::unordered_map<RiskCommandId, count>`. When the
  count reaches `shard_count`, send `CommandApplied{id}` and erase the entry.
- **Heartbeats:** every 1 s upstream. If nothing is received for 5 s, cancel
  the session.
- **Reconnect:** after `OnDone`, schedule a new session with capped
  exponential back-off (100 ms × 2ⁿ, max 5 s, reset after a successful
  accept). Link transitions broadcast `RiskLinkStatus` (already implemented;
  keep it exactly-once per transition). `stop()` cancels any pending
  reconnect and returns promptly.
- **Fuzzer:** `exchange-core/fuzz/risk_command_decode_fuzzer.cpp`: arbitrary
  bytes → `v1::RiskCommand` → `codec::decode`. Smoke-registered in CTest
  (label `fuzz`).

## Acceptance criteria

Tests in `exchange-core/tests/risk_client/risk_client_test.cpp` (label
`grpc`), using an **in-process fake sentinel**: a
`v1::RiskSentinelService::CallbackService` in the test on a loopback port,
and a fake `CommandIngress` recording broadcasts:

1. Handshake: the fake receives `SessionHello` with the configured
   `exchange_id` and `shard_count`.
2. Feeding a synthetic `PublishedEvent{Trade}` into `on_events` makes the
   fake receive exactly two `ExecutionReport`s with correct sides and maker
   flags.
3. A `RiskCommand` from the fake leads to one `broadcast`. Feeding
   `shard_count` `RiskCommandApplied` events leads to exactly one
   `CommandApplied` upstream.
4. Kill the fake server, then restart it on the same port. The client
   reconnects within the back-off bound. The ingress sees
   `RiskLinkStatus{false}` then `{true}`, exactly once each.
5. `stop()` during back-off returns within 200 ms.
6. The risk codec tests pass, and the fuzz smoke test passes under
   asan-ubsan.
7. `scripts/e2e-smoke.sh` still passes. clang-tidy is clean.

## Files expected to change

- `exchange-core/adapters/risk_client/**`
- `exchange-core/adapters/codec/include/lockstep/codec/risk_codec.hpp`, `adapters/codec/src/risk_codec.cpp`, `adapters/codec/CMakeLists.txt`
- `exchange-core/tests/risk_client/**` (new), `exchange-core/tests/CMakeLists.txt` (`add_subdirectory(risk_client)`)
- `exchange-core/tests/codec/risk_codec_test.cpp` (new)
- `exchange-core/fuzz/risk_command_decode_fuzzer.cpp` (new), `exchange-core/fuzz/CMakeLists.txt`

## Out of scope

- The sentinel's logic (tasks 015/016).
- TLS/authentication.

## Dependencies

None hard: synthetic events suffice for tests. **Soft:** 004 (so that
applied commands have real effects) and 011 (subscriptions; keep using the
synchronous `EventSubscriber` path unless 011 has landed).
