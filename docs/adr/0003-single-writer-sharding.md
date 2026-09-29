# 3. Single-writer sharding with per-shard ingress and a single publisher

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

Order books are mutated by every order, cancel and fill. Protecting them with
locks puts contention and lock hand-off latency on the hottest path, and it
makes the domain aware of threads (contradicting ADR-0002). The LMAX Disruptor
line of designs avoids both: each piece of mutable state has exactly one
writer thread, and threads communicate only through queues.

Two shapes were considered:

1. **One global sequencer** (LMAX): every input goes through a single thread
   that assigns a global sequence number and journals it, then fans out to
   business-logic threads. This gives a total order of all events, but the
   sequencer is a throughput ceiling and adds a hop to every command.
2. **Per-shard sequencing**: instruments are partitioned across N shard
   threads. Each shard owns its books, has its own ingress queue, and
   sequences and journals its own inputs.

## Decision

Per-shard sequencing (option 2):

```
gRPC threads ─┐                          ┌─> SPSC ─┐
risk client ──┼─> MPSC[shard k] ─> shard k ...      ├─> publisher ─> subscribers, completions
              └─> MPSC[shard j] ─> shard j ─> SPSC ─┘
```

- **Routing.** `Router::round_robin` sorts instruments by id and deals them
  across shards. The assignment is a pure function of the instrument set, and
  the journal header records it (ADR-0012).
- **Ingress.** One bounded multi-producer queue per shard. A full queue
  returns `SubmitError::Overloaded` (gRPC `RESOURCE_EXHAUSTED`). gRPC threads
  never block on a shard.
- **Shard thread.** A `std::jthread` that:
  1. drains a batch;
  2. for each command, assigns the next shard sequence number and a timestamp
     (from the `Clock` port), appends to the journal, and applies it to its
     `ShardEngine`;
  3. commits the journal;
  4. only then pushes events and replies to its egress queue.
- **Egress.** One single-producer/single-consumer queue per shard to one
  **publisher thread**. The publisher fans events out to subscribers (market
  data, risk client) and runs completions, so shard threads never call gRPC.
- **Risk commands** are broadcast into every shard's ingress and journaled by
  each shard like any other command.
- **Shutdown protocol** (`Engine`):
  1. stop producers (the gRPC server, then the risk client);
  2. request stop on the shards, which drain their ingress and commit;
  3. join them;
  4. request stop on the publisher, which drains every egress;
  5. join it.

  Every accepted command therefore gets its completion. `std::stop_token`
  carries the stop request; `std::jthread` guarantees the join.
- **Cache-line isolation.** Producer-written and consumer-written indices of
  every queue sit on separate 128-byte lines (ADR-0011).

## Consequences

- <a id="adr0003-domain-no-synchronisation-v1"></a>The domain contains no synchronisation at all, and TSan has a small,
  well-defined surface: queues, the runtime, and the atomics in `Engine`.
- Throughput scales with shard count for workloads spread over instruments.
- **No total order across shards.** Events from different shards may
  interleave differently from run to run. Determinism is guaranteed *per
  shard* (ADR-0004). A cross-instrument feature (for example, a spread order
  spanning two shards) would need a coordination design and a new ADR.
- A trader's orders on different instruments may be processed out of
  submission order. Risk is post-trade (ADR-0013), so this is acceptable.
- The instrument→shard map cannot change while a journal is being written.
  Resharding means a new journal generation.
