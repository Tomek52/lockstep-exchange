# 13. Risk feedback loop semantics

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

risk-sentinel is a separate service, deliberately in another language, that
watches executions and can block traders or halt the exchange. Integrating
an external controller into a deterministic, sharded core raises questions:

- How does a command from outside become part of replayable state (ADR-0004)?
- How does the sentinel know a command took effect across N shards?
- What happens when the sentinel is down or the stream breaks?
- What if the sentinel sends the same command twice (reconnects, retries)?

## <a id="adr0013-decision-v1"></a>Decision

**Session.**

- exchange-core is the gRPC *client* of `RiskSentinelService.Monitor` and
  opens one bidirectional stream per exchange instance.
- The first upstream message is `SessionHello` (protocol version, exchange
  id, shard count). The first downstream message is `SessionAccept`.
- The sentinel refuses an incompatible protocol major with
  `FAILED_PRECONDITION`.

<a id="adr0013-commands-journaled-v1"></a>**Commands become journaled inputs.**

- The risk client decodes each `RiskCommand` into a domain command
  (`BlockTrader`, `UnblockTrader`, `KillSwitch`).
- It calls `CommandIngress::broadcast()`, which puts one copy into every
  shard's ingress queue.
- Each shard journals and applies its copy, then emits
  `RiskCommandApplied{command_id}`.
- `broadcast` waits for queue space instead of failing with `Overloaded`:
  risk commands are rare and must not be dropped. It fails only on shutdown.

**Acknowledgement.** The risk client aggregates `RiskCommandApplied` from all
shards and sends one `CommandApplied{command_id}` upstream once every shard
has applied it (task 014).

<a id="adr0013-idempotency-v1"></a>**Idempotency.**

- `command_id` is the idempotency key.
- Blocking an already-blocked trader and engaging an engaged kill switch are
  no-ops that still acknowledge.
- The sentinel emits each action once per breach, not on every subsequent
  fill (task 015).

<a id="adr0013-link-status-input-v1"></a>**Link status is an input.**

- Transitions of the session (accepted → closed) are broadcast as
  `RiskLinkStatus{connected}` commands and journaled.
- Policy (`RiskLinkPolicy`):
  - **FailOpen** (default): keep trading while the sentinel is unreachable.
    Risk is post-trade; halting the market because a monitor is down is
    worse.
  - <a id="adr0013-fail-closed-policy-v1"></a>**FailClosed**: reject new orders with `RISK_UNAVAILABLE` while
    disconnected.
- Because link status is journaled, replay reproduces exactly which orders
  were rejected under FailClosed.

**Reconnect.** The risk client reconnects with capped exponential back-off
(task 014). The sentinel's position state survives exchange reconnects
because it is keyed by trader, not by session.

## Consequences

- Blocks and kill switches are replayable and auditable like orders.
- A kill switch takes effect per shard at that shard's next command boundary.
  There is no global atomic halt, consistent with ADR-0003. The
  `CommandApplied` ack tells the sentinel when *all* shards have halted.
- The sentinel must tolerate duplicate execution reports after a reconnect.
  It detects them by `(shard_id, shard_sequence)`, since execution reports
  carry both.
- The skeleton implements the handshake, inbound command broadcast and link
  status. Report forwarding, ack aggregation and reconnect are task 014.
