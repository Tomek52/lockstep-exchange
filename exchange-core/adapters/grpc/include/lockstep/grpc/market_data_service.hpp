#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "lockstep/app/engine.hpp"
#include <lockstep/v1/market_data.grpc.pb.h>

namespace lockstep::grpc_adapter {

/// Driving adapter: MarketDataService.Subscribe over the gRPC callback API.
///
/// Each call registers an app::Subscription with the engine (task 011) and
/// streams its events as SubscribeResponse messages, one event per message,
/// in per-shard sequence order. Only public data is sent: trades, price level
/// changes and instrument status, never trader or order ids.
///
/// Request filter: `instrument_ids` empty means every instrument.
/// `include_trades` and `include_book_updates` are plain proto3 booleans, so a
/// request that sets neither receives instrument status changes only (those
/// are not gated by any flag, see app::SubscriptionFilter).
///
/// Outcome mapping (ADR-0006):
///  * slow consumer (its bounded buffer overflowed) -> RESOURCE_EXHAUSTED, sent
///    after the events that were already buffered;
///  * protocol major version mismatch               -> FAILED_PRECONDITION;
///  * server shutting down (close_streams())        -> UNAVAILABLE;
///  * client cancelled                              -> the call ends.
///
/// Threading. The publisher thread must never wait for gRPC, and gRPC may run
/// a completion callback inline on the thread that started the operation. So
/// the subscription's on_ready() hook does nothing but set a flag and wake one
/// dispatcher thread (lock-free); that thread, and gRPC's own threads from
/// OnWriteDone, are the only ones that start writes. A stream's mutable state
/// is touched by one thread at a time through a hand-over flag (`pumping_`).
/// Only the dispatcher destroys a stream, after gRPC's OnDone.
///
/// A client that stops reading entirely keeps its stream open until it
/// disconnects, because gRPC cannot finish a call with a write outstanding;
/// its memory is bounded (one ring buffer plus gRPC's flow-control window)
/// and the publisher is never slowed by it.
class MarketDataService final : public v1::MarketDataService::CallbackService {
public:
    /// `per_subscriber_capacity` is each stream's buffer in events (rounded up
    /// to a power of two, see app::Subscription).
    MarketDataService(app::Engine& engine, std::size_t per_subscriber_capacity);
    /// The gRPC server must already be shut down (GrpcServer::shutdown) so
    /// that every stream has completed; calls fatal() otherwise.
    ~MarketDataService() override;
    MarketDataService(const MarketDataService&) = delete;
    MarketDataService& operator=(const MarketDataService&) = delete;
    MarketDataService(MarketDataService&&) = delete;
    MarketDataService& operator=(MarketDataService&&) = delete;

    grpc::ServerWriteReactor<v1::SubscribeResponse>* Subscribe(
        grpc::CallbackServerContext* context, const v1::SubscribeRequest* request) override;

    /// Ends every active stream with UNAVAILABLE. Call before
    /// GrpcServer::shutdown() so that shutdown does not have to wait out its
    /// grace period for streams that would never end on their own.
    void close_streams() noexcept;

    /// Streams whose subscription is registered with the engine and that have
    /// not completed yet. Once it counts a stream, that stream misses no event
    /// from commands that start afterwards; tests wait on this before
    /// submitting orders.
    [[nodiscard]] std::size_t active_streams() const;

private:
    class Stream;

    void signal() noexcept;
    void run(const std::stop_token& stop);
    void sweep();

    app::Engine& engine_;
    std::size_t capacity_;

    mutable std::mutex streams_mutex_;
    std::vector<Stream*> streams_;  // guarded by streams_mutex_; deleted by the dispatcher only

    std::atomic<bool> signalled_{false};
    std::jthread dispatcher_;  // last member: started after everything above exists
};

}  // namespace lockstep::grpc_adapter
