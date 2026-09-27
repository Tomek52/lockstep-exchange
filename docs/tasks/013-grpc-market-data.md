# 013: `MarketDataService.Subscribe`

## Goal

Serve public market data (trades, price-level updates, instrument status)
over the server-streaming RPC defined in `proto/lockstep/v1/market_data.proto`,
using the subscriptions from task 011.

## Context

- Contract: `MarketDataService.Subscribe(SubscribeRequest) returns (stream SubscribeResponse)`.
  Read the comments in `market_data.proto`.
- [ADR-0006](../adr/0006-grpc-service-and-stream-design.md):
  - slow consumers are disconnected with `RESOURCE_EXHAUSTED`;
  - events carry `(shard_id, shard_sequence)`;
  - protocol version is in `SubscribeRequest.protocol`;
  - a major mismatch → `FAILED_PRECONDITION`.
- Task 011 provides
  `Engine::subscribe(SubscriptionFilter, capacity) -> shared_ptr<Subscription>`
  with `poll`, `overflowed`, `cancel`, `on_ready`.
- Existing adapter pattern: `OrderEntryService`
  (`exchange-core/adapters/grpc/src/order_entry_service.cpp`) uses the gRPC
  **callback API**. Use `grpc::ServerWriteReactor<SubscribeResponse>` here.
- Public data only: trader and order ids must **not** appear. Map domain
  `Trade` → proto `Trade{instrument_id, price_ticks, quantity, aggressor_side}`.
- Namespace: `lockstep::grpc_adapter`. A namespace called `grpc` would shadow
  `::grpc` inside `lockstep`.

## Interfaces to implement

```cpp
// adapters/codec/include/lockstep/codec/market_data_codec.hpp
/// Returns false for events that are not public market data.
[[nodiscard]] bool encode(const app::PublishedEvent& event, v1::SubscribeResponse& out);

// adapters/grpc/include/lockstep/grpc/market_data_service.hpp
class MarketDataService final : public v1::MarketDataService::CallbackService {
public:
    MarketDataService(app::Engine& engine, std::size_t per_subscriber_capacity);
    grpc::ServerWriteReactor<v1::SubscribeResponse>* Subscribe(
        grpc::CallbackServerContext*, const v1::SubscribeRequest*) override;
};
```

The reactor:

- keeps at most one outstanding `StartWrite`;
- on `on_ready` from the publisher thread, schedules the next write without
  blocking;
- on `overflowed()`, finishes with `RESOURCE_EXHAUSTED`;
- on client cancel (`OnCancel`), cancels the subscription;
- deletes itself in `OnDone`.

Register the service in `exchange-core/main/src/main.cpp`.

## Acceptance criteria

1. **Codec tests** (`tests/codec/market_data_codec_test.cpp`): `Trade`,
   `BookLevelChanged` and `InstrumentStatusChanged` map field by field.
   Private events return `false`, and no trader id appears in any output.
2. **In-process gRPC tests** (`tests/grpc/market_data_service_test.cpp`),
   with a real `Engine` and loopback server:
   - a subscriber receives events in shard sequence order after an order is
     submitted;
   - the instrument filter works;
   - a client that stops reading with a small buffer (for example capacity 4)
     gets `RESOURCE_EXHAUSTED` while a second subscriber keeps receiving;
   - a protocol major of 2 → `FAILED_PRECONDITION`.
3. Shutdown with active subscribers completes (no hang). Test with a timeout.
4. Labels: these tests are `grpc`, so they are excluded from tsan.
   asan-ubsan and debug pass.
5. clang-tidy is clean.

## Files expected to change

- `exchange-core/adapters/codec/include/lockstep/codec/market_data_codec.hpp`, `adapters/codec/src/market_data_codec.cpp`, `adapters/codec/CMakeLists.txt`
- `exchange-core/adapters/grpc/include/lockstep/grpc/market_data_service.hpp`, `adapters/grpc/src/market_data_service.cpp`, `adapters/grpc/CMakeLists.txt`
- `exchange-core/main/src/main.cpp`
- `exchange-core/tests/codec/*`, `exchange-core/tests/grpc/*` (new files + CMakeLists)

## Out of scope

- Book snapshots on subscribe. Conflation. Per-client rate limits.

## Dependencies

- **Hard:** 011.
