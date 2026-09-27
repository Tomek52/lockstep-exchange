# Architecture

Lockstep is a small exchange simulator with two services:

- **exchange-core** (C++23): order entry, matching, market data, and the
  write-ahead journal that makes every state replayable.
- **risk-sentinel** (Rust): watches executions and pushes risk commands
  (block a trader, kill switch) back into the exchange.

Read in this order:

1. [C4 context](c4-context.md): who uses the system and what it talks to.
2. [C4 containers](c4-container.md): the processes, their protocols, and the
   component view of exchange-core's hexagonal layout.
3. [Threading and data flow](threading-dataflow.md): the single-writer runtime,
   the life of an order, and the shutdown protocol.
4. [Domain model](domain-model.md): commands, events, and the determinism
   contract.

The *why* behind each structure is in the [ADRs](../adr/README.md). Diagrams
use Mermaid flowcharts styled after the C4 model (GitHub renders them
natively).

## Quality attributes, in priority order

| Attribute | How the architecture serves it | ADR |
|---|---|---|
| **Determinism / auditability** | Per-shard write-ahead journal; time and risk commands are journaled inputs; domain free of clocks and I/O | [0004](../adr/0004-deterministic-replay-via-per-shard-journal.md) |
| **Testability** | Pure domain behind ports; one test executable per layer; architecture fitness functions | [0002](../adr/0002-hexagonal-architecture-enforced-by-the-build.md), [0014](../adr/0014-testing-strategy.md) |
| **Latency** | Single writer per book (no locks); bounded lock-free queues; batching amortises journal commits | [0003](../adr/0003-single-writer-sharding.md), [0011](../adr/0011-lock-free-queues-and-memory-ordering.md) |
| **Evolvability** | One proto source of truth, `buf breaking` in CI, runtime-negotiated minor versions | [0006](../adr/0006-grpc-service-and-stream-design.md) |
| **Operability** | Docker Compose stack; clean, lossless shutdown; fail-open risk link by default | [0013](../adr/0013-risk-feedback-loop.md) |
