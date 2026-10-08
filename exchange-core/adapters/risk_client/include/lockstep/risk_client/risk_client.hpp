#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>

#include "lockstep/app/ports/command_ingress.hpp"
#include "lockstep/app/ports/event_subscriber.hpp"

namespace lockstep::v1 {
class RiskCommand;     // generated; forward-declared to keep gRPC out of this header
class MonitorRequest;  // generated
}  // namespace lockstep::v1

namespace lockstep::risk_client {

struct RiskClientConfig {
    std::string target;  ///< host:port of risk-sentinel
    std::string exchange_id;
    std::uint32_t shard_count{1};

    /// How often a heartbeat is sent upstream while a session is up.
    std::chrono::milliseconds heartbeat_interval{1000};
    /// Cancel the session if nothing is received for this long (ADR-0013).
    std::chrono::milliseconds receive_timeout{5000};
    /// Capped exponential back-off between reconnect attempts: base, then
    /// base*2^n, clamped to max; reset after a successful accept.
    std::chrono::milliseconds backoff_base{100};
    std::chrono::milliseconds backoff_max{5000};
    /// Upstream write queue bound; on overflow the session is dropped and a
    /// reconnect scheduled (never block the publisher).
    std::size_t max_queued_messages{100'000};
};

/// Driven + driving adapter for the risk loop (ADR-0013). Keeps one
/// RiskSentinelService.Monitor bidi stream alive, reconnecting with back-off:
///  * upstream: hello, then execution reports (two per Trade), CommandApplied
///    acks aggregated across shards, and periodic heartbeats;
///  * downstream: accept, then RiskCommands, decoded and broadcast to every
///    shard through CommandIngress so they are journaled like orders.
/// Link transitions (accepted <-> closed) are broadcast as RiskLinkStatus
/// commands, exactly once per transition, so they are journaled too.
class RiskClient final : public app::EventSubscriber {
public:
    RiskClient(RiskClientConfig config, app::CommandIngress& ingress);
    RiskClient(const RiskClient&) = delete;
    RiskClient& operator=(const RiskClient&) = delete;
    RiskClient(RiskClient&&) = delete;
    RiskClient& operator=(RiskClient&&) = delete;
    ~RiskClient() override;

    /// Starts the manager thread, which opens the stream and keeps it alive.
    /// An unreachable sentinel is logged, not fatal (fail-open, ADR-0013).
    void start();

    /// Stops the manager thread, cancels any live stream and any pending
    /// reconnect back-off, and returns promptly. Call before Engine::stop() -
    /// the risk client is a producer (shutdown protocol).
    void stop();

    [[nodiscard]] bool session_established() const noexcept;

    /// Publisher-thread callback: turns Trades into execution reports and
    /// aggregates RiskCommandApplied across shards. Never blocks (ADR-0006).
    void on_events(std::span<const app::PublishedEvent> events) override;

private:
    class Session;
    friend class Session;

    void run();  ///< manager-thread loop: connect, then reconnect with back-off
    void enqueue_upstream(v1::MonitorRequest&& message);  ///< thread-safe; may drop the session

    void on_accept(const std::string& sentinel_id);
    void on_command(const v1::RiskCommand& command);
    void on_session_done(bool was_established, const std::string& reason);

    RiskClientConfig config_;
    app::CommandIngress& ingress_;

    std::atomic<bool> established_{false};

    std::thread manager_;

    // Guards all mutable session state below. Held briefly; never while
    // blocking on gRPC.
    std::mutex mutex_;
    std::condition_variable wake_;  ///< manager wakes on: session done, or stop
    bool stopping_{false};
    bool session_done_{false};          ///< the current session finished (reconnect)
    std::unique_ptr<Session> session_;  ///< the live stream, or null between attempts
    std::unordered_map<std::uint64_t, std::uint32_t> applied_counts_;  ///< command_id -> shards
};

}  // namespace lockstep::risk_client
