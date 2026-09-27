# Threading and data flow

The runtime follows the single-writer principle ([ADR-0003](../adr/0003-single-writer-sharding.md)):
every order book has exactly one thread that ever touches it, so the domain
needs no locks. Threads talk only through bounded queues
([ADR-0011](../adr/0011-lock-free-queues-and-memory-ordering.md)).

## Threads and queues

```mermaid
flowchart LR
    subgraph io["gRPC callback threads (many)"]
        g1["OrderEntry<br/>handler"]
        g2["OrderEntry<br/>handler"]
    end
    rc["risk client<br/>read callback"]

    subgraph s0["shard 0 thread  (std::jthread)"]
        direction TB
        seq0["sequence + timestamp"] --> j0["journal.append"] --> a0["ShardEngine::apply"]
        a0 --> c0["journal.commit<br/>then release batch"]
    end
    subgraph s1["shard 1 thread  (std::jthread)"]
        direction TB
        seq1["sequence + timestamp"] --> j1["journal.append"] --> a1["ShardEngine::apply"]
        a1 --> c1["journal.commit<br/>then release batch"]
    end

    q0[["MPSC ingress 0<br/>bounded"]]
    q1[["MPSC ingress 1<br/>bounded"]]
    e0[["SPSC egress 0"]]
    e1[["SPSC egress 1"]]

    pub["publisher thread<br/>(std::jthread)"]
    md["market data<br/>subscribers"]
    rs["risk client<br/>(EventSubscriber)"]
    done["completions<br/>(reactor->Finish)"]

    g1 -- "route by instrument" --> q0
    g2 -- "route by instrument" --> q1
    rc -- "broadcast" --> q0 & q1
    q0 --> s0
    q1 --> s1
    s0 --> e0 --> pub
    s1 --> e1 --> pub
    pub --> md & rs & done
```

| Thread | Owns (sole writer) | Reads from | Writes to |
|---|---|---|---|
| gRPC callback threads | nothing | network | shard ingress (MPSC) |
| risk client callbacks | nothing | Monitor stream | every shard ingress (broadcast) |
| shard *k* | `ShardEngine` *k* (books, risk state), journal *k*, sequence counter | ingress *k* | egress *k* (SPSC) |
| publisher | subscriber buffers | all egress queues | subscribers, completions |

## Life of an order

```mermaid
sequenceDiagram
    autonumber
    participant C as client (loadgen)
    participant G as gRPC thread<br/>OrderEntryService
    participant I as ingress[k] (MPSC)
    participant S as shard k thread
    participant J as journal[k]
    participant E as egress[k] (SPSC)
    participant P as publisher thread

    C->>G: SubmitOrder(request)
    G->>G: check protocol header, then codec::decode
    alt malformed / unknown instrument
        G-->>C: OK + ack.rejected (shard_sequence = 0)
    else queue full
        G-->>C: RESOURCE_EXHAUSTED
    else
        G->>I: try_push(InboundCommand{cmd, completion})
        Note over G: handler returns immediately,<br/>no gRPC thread waits
        S->>I: try_pop (batch)
        S->>S: seq = ++sequence, ts = clock.now()
        S->>J: append(SequencedCommand)
        S->>S: engine.apply → result + events
        S->>J: commit() (end of batch)
        S->>E: push events, then ReplyTask
        P->>E: try_pop
        P->>P: fan out events to subscribers
        P->>G: completion(reply): encode ack, reactor->Finish(OK)
        G-->>C: SubmitOrderResponse{ack}
    end
```

The ack is never sent before the command is journaled: outputs are released
only after `commit()` ([ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md)).

## Shutdown protocol

Lossless shutdown requires stopping producers before consumers:

```mermaid
sequenceDiagram
    participant M as main (sigwait)
    participant GS as GrpcServer
    participant RC as RiskClient
    participant EN as Engine
    participant SH as shard threads
    participant PU as publisher

    M->>M: SIGTERM received
    M->>GS: shutdown(2 s grace)
    Note over GS: in-flight RPCs complete<br/>(their completions still run)
    M->>RC: stop(): cancel stream, wait OnDone
    Note over RC: link-down journaled as RiskLinkStatus
    M->>EN: stop()
    EN->>SH: request_stop (std::stop_token)
    SH->>SH: drain ingress, commit journal
    EN->>SH: join (std::jthread)
    EN->>PU: request_stop
    PU->>PU: drain every egress, run completions
    EN->>PU: join
    M->>M: "exchange-core stopped cleanly", exit 0
```

Signals are blocked in every thread (`pthread_sigmask` before any thread
starts) and consumed synchronously by `sigwait` in `main`. No asynchronous
signal handler ever runs concurrently with the runtime.

## Memory-ordering map

| Atomic | Location | Ordering | Why |
|---|---|---|---|
| `Engine::accepting_` | `app/src/engine.cpp` | relaxed | Admission hint only; producers are stopped before `stop()` |
| `ManualClock::next_` | `app/ports/clock.hpp` | relaxed | Only uniqueness of values matters |
| fatal handler pointer | `app/src/fatal.cpp` | relaxed | Function pointer, no associated data |
| `RiskClient::established_` | `adapters/risk_client` | release / acquire | Publishes the accept handler's effects |
| SPSC `head_`/`tail_` | task 005 | acquire/release pairs | See ADR-0011 |
| MPSC slot sequences, `enqueue_pos_` | task 006 | acquire/release + relaxed CAS | See ADR-0011 |
