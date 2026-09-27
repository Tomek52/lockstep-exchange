#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "lockstep/app/ports/command_ingress.hpp"
#include "lockstep/app/ports/event_subscriber.hpp"

namespace lockstep::v1 {
class RiskCommand;  // generated; forward-declared to keep gRPC out of this header
}  // namespace lockstep::v1

namespace lockstep::risk_client {

struct RiskClientConfig {
    std::string target;  ///< host:port of risk-sentinel
    std::string exchange_id;
    std::uint32_t shard_count{1};
};

/// Driven + driving adapter for the risk loop (ADR-0013). Holds one
/// RiskSentinelService.Monitor bidi stream:
///  * upstream: hello, then (task 014) execution reports and CommandApplied acks
///    derived from published events;
///  * downstream: accept, then RiskCommands, decoded and broadcast to every
///    shard through CommandIngress so they are journaled like orders.
/// Link transitions are themselves broadcast as RiskLinkStatus commands.
///
/// SKELETON STATUS: handshake + inbound commands. Report forwarding, ack
/// aggregation and reconnect-with-backoff: docs/tasks/014-risk-client-adapter.md.
class RiskClient final : public app::EventSubscriber {
public:
    RiskClient(RiskClientConfig config, app::CommandIngress& ingress);
    RiskClient(const RiskClient&) = delete;
    RiskClient& operator=(const RiskClient&) = delete;
    RiskClient(RiskClient&&) = delete;
    RiskClient& operator=(RiskClient&&) = delete;
    ~RiskClient() override;

    /// Opens the stream asynchronously. An unreachable sentinel is logged, not
    /// fatal (fail-open by default, ADR-0013).
    void start();

    /// Cancels the stream and waits until gRPC reports it done. Call before
    /// Engine::stop() - the risk client is a producer (shutdown protocol).
    void stop();

    [[nodiscard]] bool session_established() const noexcept;

    void on_events(std::span<const app::PublishedEvent> events) override;

private:
    class Session;
    friend class Session;

    void on_accept(const std::string& sentinel_id);
    void on_command(const v1::RiskCommand& command);
    void on_session_done(bool was_established, const std::string& reason);

    RiskClientConfig config_;
    app::CommandIngress& ingress_;
    std::unique_ptr<Session> session_;
    std::atomic<bool> established_{false};
};

}  // namespace lockstep::risk_client
