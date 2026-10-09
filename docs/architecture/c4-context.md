# C4 level 1: system context

```mermaid
flowchart TB
    trader["<b>Trader / Load generator</b><br/><i>[Person / automated client]</i><br/>Submits, cancels and modifies orders"]
    mdclient["<b>Market data consumer</b><br/><i>[Person / automated client]</i><br/>Watches trades and price levels"]
    operator["<b>Operator</b><br/><i>[Person]</i><br/>Runs the stack, replays journals, reads ADRs"]

    subgraph lockstep["Lockstep exchange simulator [software system]"]
        direction LR
        core["<b>exchange-core</b><br/>order entry, matching,<br/>market data, journal"]
        sentinel["<b>risk-sentinel</b><br/>positions, PnL, limits"]
    end

    trader -- "orders<br/>[gRPC, unary]" --> core
    core -- "trades, book updates<br/>[gRPC, server stream]" --> mdclient
    core <-- "execution reports ↑ / risk commands ↓<br/>[gRPC, bidirectional stream]" --> sentinel
    operator -. "start/stop, replay,<br/>docker compose" .-> lockstep

    classDef person fill:#08427b,color:#fff,stroke:#052e56
    classDef system fill:#1168bd,color:#fff,stroke:#0b4884
    class trader,mdclient,operator person
    class core,sentinel system
    style lockstep fill:none,stroke:#1168bd,stroke-dasharray:5 5
```

## Scope

| In scope | Out of scope |
|---|---|
| Continuous limit order book per instrument, price-time priority | Auctions, circuit breakers, iceberg/stop orders |
| Limit (GTC/IOC), market, cancel, modify | Authentication, entitlements, TLS |
| Post-trade risk loop with block/kill switch | Clearing, settlement, margining |
| Deterministic replay from a journal | Multi-node replication / failover |

## External interfaces

All interfaces are defined once in [`proto/lockstep/v1`](../../proto/lockstep/v1):

- `OrderEntryService`: unary `SubmitOrder` / `CancelOrder` / `ModifyOrder`
  returning a `CommandAck` stamped with `(shard_id, shard_sequence)`.
- `MarketDataService.Subscribe`: server stream of trades, level updates and
  instrument status, one event per message, public data only
  ([task 013](../tasks/013-grpc-market-data.md)).
- `RiskSentinelService.Monitor`: the bidirectional risk session
  ([ADR-0013](../adr/0013-risk-feedback-loop.md)).
