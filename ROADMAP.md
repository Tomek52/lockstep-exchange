# Roadmap

Milestones group the [task specs](docs/tasks/README.md) into shippable
increments. Within a milestone, tasks without mutual dependencies can run in
parallel. The dependency graph and file-ownership rules are in
[docs/tasks/README.md](docs/tasks/README.md).

## M0: Walking skeleton ✅

Everything needed to develop safely:
- build, presets, CI, Docker, linters;
- contracts and codegen on both sides;
- domain types and validation;
- a threaded runtime on placeholder queues;
- order entry end to end, with the risk session handshake;
- ADRs 0001–0014, and this backlog.

## M1: Matching core

The exchange actually trades.

- [x] [001 Order book storage: pooled FIFO, O(1) cancel](docs/tasks/001-order-book-storage.md)
- [x] [002 Price-time matching for limit and market orders](docs/tasks/002-matching-limit-market.md)
- [x] [003 Modify (cancel/replace) and duplicate client ids](docs/tasks/003-cancel-modify-duplicates.md)
- [x] [004 Risk controls: block, kill switch, link policy](docs/tasks/004-risk-controls-in-domain.md)

## M2: Lock-free runtime

The single-writer runtime on its real queues, with honest idle behaviour.

- [x] [005 Lock-free SPSC queue](docs/tasks/005-spsc-queue.md)
- [x] [006 Lock-free bounded MPSC queue](docs/tasks/006-mpsc-queue.md)
- [x] [007 Parking idle strategy, wake-ups, runtime stats](docs/tasks/007-runtime-idle-and-stats.md)
- [x] [011 Publisher subscriptions with slow-consumer policy](docs/tasks/011-publisher-fanout.md)
- [ ] [020 Stabilise the idle-parking test under CPU load](docs/tasks/020-stabilize-idle-park-test.md)

## M3: Durable and replayable

Every state reproducible from disk.

- [x] [008 Journal record codec, CRC32C, file writer](docs/tasks/008-journal-writer.md)
- [x] [009 Lazy journal reader, recovery, fuzzer](docs/tasks/009-journal-reader.md)
- [x] [012 Instrument reference data and config file](docs/tasks/012-instrument-config.md)
- [x] [010 File-based replay tool and full determinism suite](docs/tasks/010-deterministic-replay.md)

## M4: Risk loop and market data

The two services cooperate; clients can watch the market.

- [x] [014 Risk client: reports, ack aggregation, reconnect](docs/tasks/014-risk-client-adapter.md)
- [x] [015 Sentinel positions, PnL and limit engine](docs/tasks/015-sentinel-positions-pnl.md)
- [ ] [016 Sentinel Monitor session logic](docs/tasks/016-sentinel-monitor-service.md)
- [ ] [013 MarketDataService.Subscribe](docs/tasks/013-grpc-market-data.md)

## M5: Showcase

Numbers and a demo.

- [ ] [017 Load generator: concurrency, rate, scenarios](docs/tasks/017-loadgen.md)
- [ ] [018 Benchmarks with latency percentiles](docs/tasks/018-benchmarks.md)
- [ ] [019 End-to-end kill-switch scenario](docs/tasks/019-e2e-kill-switch.md)

## Recommended starting order

Three tasks with no dependencies, in different layers and languages, each
unblocking a lot:

1. **001:** unblocks matching (002) and risk controls (004), the critical
   path.
2. **008:** unblocks the journal reader (009) and replay (010), the core
   architectural property.
3. **015:** the Rust side; unblocks the sentinel service (016), and can run in
   parallel with everything above.

## Beyond M5 (ideas, not yet specified)

- Snapshots/checkpoints to bound replay time (needs an ADR).
- Book snapshot on market data subscribe.
- Self-trade prevention.
- C++20 modules for the domain once the toolchain allows it
  ([ADR-0010](docs/adr/0010-no-cpp20-modules-for-now.md)).
- Persisted fuzz corpora in CI.
