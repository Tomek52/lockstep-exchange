#include "lockstep/risk_client/risk_client.hpp"

#include <condition_variable>
#include <mutex>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "lockstep/codec/order_entry_codec.hpp"
#include "lockstep/support/log.hpp"
#include <lockstep/v1/risk.grpc.pb.h>

namespace lockstep::risk_client {

/// One Monitor stream. gRPC invokes the On* callbacks on its own threads,
/// serially for reads and for writes respectively.
class RiskClient::Session final
    : public grpc::ClientBidiReactor<v1::MonitorRequest, v1::MonitorResponse> {
public:
    Session(RiskClient& owner, const RiskClientConfig& config)
        : owner_{owner},
          channel_{grpc::CreateChannel(config.target, grpc::InsecureChannelCredentials())},
          stub_{v1::RiskSentinelService::NewStub(channel_)} {
        auto* hello = hello_.mutable_hello();
        hello->mutable_protocol()->set_major(1);
        hello->mutable_protocol()->set_minor(0);
        hello->set_exchange_id(config.exchange_id);
        hello->set_shard_count(config.shard_count);

        stub_->async()->Monitor(&context_, this);
        StartWrite(&hello_);
        StartRead(&response_);
        StartCall();
    }

    void OnWriteDone(bool /*ok*/) override {
        // TODO(task-014): pop the next queued upstream message (reports, acks).
    }

    void OnReadDone(bool ok) override {
        if (!ok) {
            return;  // stream is ending; OnDone follows
        }
        switch (response_.message_case()) {
            case v1::MonitorResponse::kAccept:
                established_ = true;
                owner_.on_accept(response_.accept().sentinel_id());
                break;
            case v1::MonitorResponse::kCommand:
                owner_.on_command(response_.command());
                break;
            case v1::MonitorResponse::kHeartbeat:
            case v1::MonitorResponse::MESSAGE_NOT_SET:
                break;
        }
        StartRead(&response_);
    }

    void OnDone(const grpc::Status& status) override {
        owner_.on_session_done(established_, status.error_message());
        {
            const std::scoped_lock lock{mutex_};
            done_ = true;
        }
        done_cv_.notify_all();
    }

    void cancel() { context_.TryCancel(); }

    void wait_done() {
        std::unique_lock lock{mutex_};
        done_cv_.wait(lock, [this] { return done_; });
    }

private:
    RiskClient& owner_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<v1::RiskSentinelService::Stub> stub_;
    grpc::ClientContext context_;
    v1::MonitorRequest hello_;
    v1::MonitorResponse response_;
    bool established_{false};  // touched only from read callbacks and OnDone (serialised by gRPC)
    std::mutex mutex_;
    std::condition_variable done_cv_;
    bool done_{false};
};

RiskClient::RiskClient(RiskClientConfig config, app::CommandIngress& ingress)
    : config_{std::move(config)}, ingress_{ingress} {}

RiskClient::~RiskClient() {
    stop();
}

void RiskClient::start() {
    support::info("risk: connecting to risk-sentinel at {}", config_.target);
    session_ = std::make_unique<Session>(*this, config_);
}

void RiskClient::stop() {
    if (session_) {
        session_->cancel();
        session_->wait_done();
        session_.reset();
    }
}

bool RiskClient::session_established() const noexcept {
    // acquire: pairs with the release in on_accept, so a caller that sees true
    // also sees everything the accept handler did before publishing the flag.
    return established_.load(std::memory_order_acquire);
}

void RiskClient::on_events(std::span<const app::PublishedEvent> /*events*/) {
    // TODO(task-014): turn Trade events into two ExecutionReports (maker, taker)
    // and aggregate RiskCommandApplied across shards into one CommandApplied.
}

void RiskClient::on_accept(const std::string& sentinel_id) {
    support::info("risk: session accepted by sentinel '{}'", sentinel_id);
    established_.store(true, std::memory_order_release);
    if (!ingress_.broadcast(domain::RiskLinkStatus{.connected = true})) {
        support::warn("risk: could not journal link-up (shutting down)");
    }
}

void RiskClient::on_command(const v1::RiskCommand& command) {
    auto decoded = codec::decode(command);
    if (!decoded) {
        support::warn("risk: ignoring command {}: {}", command.command_id(),
                      codec::to_string(decoded.error()));
        return;
    }
    support::info("risk: command {} received ({})", command.command_id(), command.reason());
    if (!ingress_.broadcast(std::move(*decoded))) {
        support::warn("risk: command {} dropped (shutting down)", command.command_id());
    }
}

void RiskClient::on_session_done(bool was_established, const std::string& reason) {
    established_.store(false, std::memory_order_release);
    if (was_established) {
        support::warn("risk: session closed: {}", reason);
        if (!ingress_.broadcast(domain::RiskLinkStatus{.connected = false})) {
            support::warn("risk: could not journal link-down (shutting down)");
        }
    } else {
        support::warn("risk: risk-sentinel unavailable ({}); continuing fail-open", reason);
    }
}

}  // namespace lockstep::risk_client
