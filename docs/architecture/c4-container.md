# C4 level 2: containers (and level 3 for exchange-core)

## Containers

```mermaid
flowchart LR
    loadgen["<b>loadgen</b><br/><i>[Rust binary, tonic client]</i><br/>Submits orders, reports<br/>p50/p99/p99.9 latency"]
    mdc["<b>market data client</b><br/><i>[any gRPC client]</i>"]

    subgraph core_box["exchange-core  [C++23 process]"]
        core["gRPC server :50051<br/>engine: N shard threads + publisher<br/>risk client"]
        journal[("<b>shard journals</b><br/><i>[files, own binary format]</i><br/>one per shard")]
    end

    subgraph rust_box["risk-sentinel  [Rust process, tokio + tonic]"]
        sentinel["gRPC server :50052<br/>Monitor sessions<br/>RiskEngine (pure)"]
    end

    proto{{"<b>proto/lockstep/v1</b><br/><i>[shared contracts]</i><br/>protoc (C++) · tonic-prost-build (Rust)"}}

    loadgen -- "SubmitOrder …<br/>gRPC unary" --> core
    core -- "Subscribe<br/>gRPC server-stream" --> mdc
    core -- "Monitor (client)<br/>gRPC bidi" --> sentinel
    core -- "append / commit<br/>write-ahead" --> journal

    proto -. codegen .-> core_box
    proto -. codegen .-> rust_box
    proto -. codegen .-> loadgen

    classDef ext fill:#999,color:#fff,stroke:#666
    classDef cont fill:#438dd5,color:#fff,stroke:#2e6295
    class loadgen,mdc ext
    class core,sentinel cont
```

Deployment: [`deploy/docker-compose.yml`](../../deploy/docker-compose.yml) runs
`risk-sentinel`, then `exchange-core` (once the sentinel is healthy), with
`loadgen` behind the `load` profile.

## Components of exchange-core (hexagonal layout)

Arrows are compile-time dependencies. They all point inward, and the build
enforces that ([ADR-0002](../adr/0002-hexagonal-architecture-enforced-by-the-build.md)).

```mermaid
flowchart TB
    subgraph adapters["Adapters  (may use gRPC, protobuf, files)"]
        direction LR
        grpc["<b>grpc</b><br/>OrderEntryService<br/>MarketDataService (task 013)<br/>GrpcServer"]
        risk["<b>risk_client</b><br/>Monitor session<br/>→ broadcast risk commands"]
        codec["<b>codec</b><br/>proto ⇄ domain<br/>(fuzzed)"]
        jrnl["<b>journal</b><br/>binary format, MemoryJournal,<br/>File journal (tasks 008/009)"]
    end

    subgraph app["Application  (threads + queues, no I/O)"]
        direction LR
        ports["<b>ports</b><br/>CommandIngress · Journal<br/>Clock · EventSubscriber"]
        runtime["<b>runtime</b><br/>Engine · Router<br/>ShardRuntime · Publisher<br/>replay()"]
    end

    conc["<b>concurrency</b><br/>queue concepts, MPSC/SPSC,<br/>MutexQueue, idle strategies"]

    subgraph domain["Domain  (standard library only)"]
        direction LR
        types["strong types<br/>commands · events"]
        book["OrderBook<br/>flat_map price levels"]
        engine["ShardEngine<br/>validation · matching · RiskState"]
    end

    main["<b>main</b><br/>composition root<br/>signals · fatal handler"]

    main --> grpc & risk & jrnl & runtime
    grpc --> codec --> ports
    risk --> codec
    grpc --> ports
    risk --> ports
    jrnl --> ports
    runtime --> ports
    runtime --> conc
    runtime --> engine
    ports --> types
    engine --> book --> types

    classDef dom fill:#1b5e20,color:#fff,stroke:#0d3311
    classDef ap fill:#1565c0,color:#fff,stroke:#0d3d73
    classDef ad fill:#6a1b9a,color:#fff,stroke:#3d0f59
    class types,book,engine dom
    class ports,runtime,conc ap
    class grpc,risk,codec,jrnl,main ad
```

| Component | CMake target | May link | Enforced by |
|---|---|---|---|
| domain | `lockstep::domain` | nothing | link allow-list + std-header allow-list |
| concurrency | `lockstep::concurrency` | Threads | link allow-list + include rule |
| app | `lockstep::app` | domain, concurrency | link allow-list + include rule |
| journal | `lockstep::journal` | app | link allow-list + include rule |
| codec | `lockstep::codec` | app, generated `lockstep::proto` | review |
| grpc | `lockstep::adapter_grpc` | app, codec, gRPC | review |
| risk_client | `lockstep::adapter_risk` | app, codec, gRPC | review |
| main | `exchange-core` | everything | – |
