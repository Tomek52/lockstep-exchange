# 6. gRPC service and stream design, protocol versioning

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

Two services in two languages share one contract. We need:

- a command path (order entry) with explicit back-pressure;
- a broadcast path (market data) that a slow client cannot stall;
- a long-lived, bidirectional risk session;
- one source of truth for the schema, compiled by CMake/protoc (C++) and by
  tonic-prost-build (Rust);
- a way to evolve the protocol without breaking deployed peers.

## Decision

**Contracts.** They live in `proto/lockstep/v1/*.proto`, package
`lockstep.v1`. Both builds compile the same files; neither side keeps a copy.

**Services.**

| Service | RPC | Shape | Why |
|---|---|---|---|
| `OrderEntryService` | `SubmitOrder`, `CancelOrder`, `ModifyOrder` | unary | One command, one ack. Simple client semantics; the ack is sent after the command is journaled and applied. |
| `MarketDataService` | `Subscribe` | server streaming | Public trades and level updates, stamped with `(shard_id, shard_sequence)` for gap detection. |
| `RiskSentinelService` | `Monitor` | bidirectional streaming | One session per exchange instance. Upstream: hello, execution reports, `CommandApplied` acks, heartbeats. Downstream: accept, risk commands, heartbeats. |

- **Per-RPC wrappers.** Each RPC has its own request and response message
  (`SubmitOrderResponse { CommandAck ack }`), so they can evolve
  independently. `buf lint` STANDARD also requires this.
- **Oneof envelopes.** The bidirectional stream uses `oneof` envelopes
  (`MonitorRequest`/`MonitorResponse`) rather than extra RPCs, so handshake,
  data and control messages share ordering and lifetime.

**Outcome mapping** (order entry):

| Situation | gRPC status | Body |
|---|---|---|
| accepted | OK | `ack.accepted.order_id` |
| domain rejection (invalid price, blocked trader, ...) | OK | `ack.rejected.reason` + `detail` |
| malformed request (unknown enum value) | OK | `ack.rejected` with `INVALID_*`, `shard_sequence = 0` (never sequenced) |
| ingress queue full | `RESOURCE_EXHAUSTED` | – (retry with back-off) |
| shutting down | `UNAVAILABLE` | – |
| incompatible protocol major | `FAILED_PRECONDITION` | – |

Business outcomes are data; transport conditions are statuses. Clients can
then retry on status codes without parsing reasons.

**Versioning.**

- **Major version** = the package suffix (`v1`). Any wire-breaking change
  requires `lockstep.v2`. CI runs `buf breaking` (WIRE_JSON) against the
  base branch.
- **Minor version** = additive features, negotiated at runtime:
  `ProtocolVersion` in `SessionHello`/`SessionAccept` for streams, and the
  `x-lockstep-protocol: <major>.<minor>` metadata header for unary calls.
  Absent means current major (lenient); a different major is refused.

**Slow consumers** (market data, task 013). Each subscriber has a bounded
buffer. A subscriber that falls behind is disconnected with
`RESOURCE_EXHAUSTED`, and the publisher never waits. Gap detection via
`shard_sequence` lets a client know it missed data and must resubscribe.

**Deferred.** A FIX-like streaming order-entry session (higher throughput,
ordered acks per session). Unary calls are enough for the load we generate,
and much easier to reason about. Revisit if loadgen shows per-call overhead
dominates.

## Consequences

- One `.proto` change updates both sides. The `proto` CI job fails before
  either build does if lint, format or compatibility break.
- Clients see back-pressure immediately, instead of timeouts.
- Unary order entry costs one HTTP/2 stream per command. That is acceptable
  now and measurable with loadgen.
- The protocol header is optional, so old clients keep working and new
  incompatible ones fail loudly.
