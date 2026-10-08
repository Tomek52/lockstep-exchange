#include "lockstep/risk_client/risk_client.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "lockstep/codec/order_entry_codec.hpp"
#include "lockstep/codec/risk_codec.hpp"
#include "lockstep/support/log.hpp"
#include <lockstep/v1/risk.grpc.pb.h>

namespace lockstep::risk_client {

namespace {

[[nodiscard]] std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

/// One Monitor stream. gRPC invokes the On* callbacks on its own threads,
/// serially per direction (reads serial; writes serial). Owns the upstream
/// write queue: exactly one StartWrite is outstanding, and OnWriteDone pulls
/// the next queued message, so the publisher thread never blocks on gRPC.
class RiskClient::Session final
    : public grpc::ClientBidiReactor<v1::MonitorRequest, v1::MonitorResponse> {
public:
    Session(RiskClient& owner, const RiskClientConfig& config)
        : owner_{owner},
          channel_{grpc::CreateChannel(config.target, grpc::InsecureChannelCredentials())},
          stub_{v1::RiskSentinelService::NewStub(channel_)} {
        auto* hello = pending_.emplace_back().mutable_hello();
        hello->mutable_protocol()->set_major(1);
        hello->mutable_protocol()->set_minor(0);
        hello->set_exchange_id(config.exchange_id);
        hello->set_shard_count(config.shard_count);

        last_received_ns_.store(now_ns(), std::memory_order_relaxed);

        stub_->async()->Monitor(&context_, this);
        // hello is already queued (write_in_flight_ starts true); kick off the
        // first write and the read loop.
        StartWrite(&pending_.front());
        StartRead(&response_);
        StartCall();
    }

    /// Appends an upstream message. Returns false if the queue is full (the
    /// caller then drops the session). Thread-safe; never blocks on gRPC.
    [[nodiscard]] bool enqueue(v1::MonitorRequest&& message, std::size_t bound) {
        const std::scoped_lock lock{write_mutex_};
        if (cancelled_ || pending_.size() >= bound) {
            return false;
        }
        pending_.push_back(std::move(message));
        if (!write_in_flight_) {
            write_in_flight_ = true;
            StartWrite(&pending_.front());
        }
        return true;
    }

    void OnWriteDone(bool ok) override {
        const std::scoped_lock lock{write_mutex_};
        if (!pending_.empty()) {
            pending_.pop_front();  // the message that just completed
        }
        if (!ok || cancelled_ || pending_.empty()) {
            write_in_flight_ = false;
            return;  // on !ok the stream is ending; OnDone follows
        }
        StartWrite(&pending_.front());
    }

    void OnReadDone(bool ok) override {
        if (!ok) {
            return;  // stream is ending; OnDone follows
        }
        last_received_ns_.store(now_ns(), std::memory_order_relaxed);
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
            const std::scoped_lock lock{done_mutex_};
            done_ = true;
        }
        done_cv_.notify_all();
    }

    void cancel() {
        {
            const std::scoped_lock lock{write_mutex_};
            cancelled_ = true;
        }
        context_.TryCancel();
    }

    void wait_done() {
        std::unique_lock lock{done_mutex_};
        done_cv_.wait(lock, [this] { return done_; });
    }

    /// Nanoseconds (steady clock) since anything was last received downstream.
    [[nodiscard]] std::int64_t idle_ns() const {
        return now_ns() - last_received_ns_.load(std::memory_order_relaxed);
    }

private:
    RiskClient& owner_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<v1::RiskSentinelService::Stub> stub_;
    grpc::ClientContext context_;
    v1::MonitorResponse response_;
    bool established_{false};  // touched only from read callbacks and OnDone (serialised by gRPC)
    std::atomic<std::int64_t> last_received_ns_{0};

    // Upstream write path: queue of outstanding+pending messages. pending_.front()
    // is the one currently being written while write_in_flight_ is true.
    std::mutex write_mutex_;
    std::deque<v1::MonitorRequest> pending_;
    bool write_in_flight_{true};  // the constructor queues hello and starts its write
    bool cancelled_{false};

    std::mutex done_mutex_;
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
    manager_ = std::thread{[this] { run(); }};
}

void RiskClient::stop() {
    {
        const std::scoped_lock lock{mutex_};
        if (stopping_) {
            return;  // idempotent (destructor may run after an explicit stop)
        }
        stopping_ = true;
    }
    wake_.notify_all();
    if (manager_.joinable()) {
        manager_.join();  // the manager tears down the live session on its way out
    }
}

bool RiskClient::session_established() const noexcept {
    // acquire: pairs with the release in on_accept, so a caller that sees true
    // also sees everything the accept handler did before publishing the flag.
    return established_.load(std::memory_order_acquire);
}

void RiskClient::run() {
    std::chrono::milliseconds backoff = config_.backoff_base;
    while (true) {
        {
            const std::scoped_lock lock{mutex_};
            if (stopping_) {
                break;
            }
            session_done_ = false;
            applied_counts_.clear();  // a new stream re-aggregates from scratch
            session_ = std::make_unique<Session>(*this, config_);
        }

        // Service the live session: send heartbeats on the interval and cancel
        // if nothing has been received within the receive timeout.
        bool established_this_session = false;
        while (true) {
            std::unique_lock lock{mutex_};
            wake_.wait_for(lock, config_.heartbeat_interval,
                           [this] { return stopping_ || session_done_; });
            if (stopping_ || session_done_) {
                break;
            }
            Session* session = session_.get();
            if (session == nullptr) {
                break;
            }
            const bool is_up = established_.load(std::memory_order_acquire);
            established_this_session = established_this_session || is_up;
            const std::int64_t idle_ns = session->idle_ns();
            lock.unlock();

            if (idle_ns > std::chrono::nanoseconds{config_.receive_timeout}.count()) {
                support::warn("risk: no message for {} ms; cancelling session",
                              config_.receive_timeout.count());
                session->cancel();
                continue;  // wait for OnDone -> session_done_
            }
            v1::MonitorRequest heartbeat;
            heartbeat.mutable_heartbeat()->set_sent_at_ns(now_ns());
            if (!session->enqueue(std::move(heartbeat), config_.max_queued_messages)) {
                support::error("risk: upstream queue full on heartbeat; dropping session");
                session->cancel();
            }
        }

        // Tear down the finished (or cancelled) session.
        std::unique_ptr<Session> finished;
        {
            const std::scoped_lock lock{mutex_};
            finished = std::move(session_);
        }
        if (finished) {
            finished->cancel();
            finished->wait_done();
            finished.reset();
        }

        {
            const std::scoped_lock lock{mutex_};
            if (stopping_) {
                break;
            }
        }

        // Capped exponential back-off; reset after a session that was accepted.
        if (established_this_session) {
            backoff = config_.backoff_base;
        }
        {
            std::unique_lock lock{mutex_};
            const bool woken = wake_.wait_for(lock, backoff, [this] { return stopping_; });
            if (woken) {  // stopping_
                break;
            }
        }
        backoff = std::min(backoff * 2, config_.backoff_max);
    }

    // Leaving: make sure no session outlives the manager thread.
    std::unique_ptr<Session> finished;
    {
        const std::scoped_lock lock{mutex_};
        finished = std::move(session_);
    }
    if (finished) {
        finished->cancel();
        finished->wait_done();
    }
}

void RiskClient::enqueue_upstream(v1::MonitorRequest&& message) {
    {
        // Hold mutex_ for the whole call so the manager cannot move out and
        // destroy the session underneath us. Session::enqueue takes a
        // different lock (write_mutex_) and never blocks on gRPC.
        const std::scoped_lock lock{mutex_};
        if (session_ == nullptr) {
            return;  // no live stream; the sentinel re-derives state after reconnect
        }
        if (session_->enqueue(std::move(message), config_.max_queued_messages)) {
            return;
        }
        session_done_ = true;  // ask the manager to tear down and reconnect
    }
    support::error("risk: upstream queue full; dropping session for reconnect");
    wake_.notify_all();
}

void RiskClient::on_events(std::span<const app::PublishedEvent> events) {
    for (const auto& event : events) {
        if (const auto* trade = std::get_if<domain::Trade>(&event.event)) {
            for (auto& report : codec::encode_execution_reports(event, *trade)) {
                v1::MonitorRequest message;
                *message.mutable_execution() = std::move(report);
                enqueue_upstream(std::move(message));
            }
        } else if (const auto* applied = std::get_if<domain::RiskCommandApplied>(&event.event)) {
            const std::uint64_t id = applied->command_id.value();
            bool complete = false;
            {
                const std::scoped_lock lock{mutex_};
                const std::uint32_t count = ++applied_counts_[id];
                if (count >= config_.shard_count) {
                    applied_counts_.erase(id);
                    complete = true;
                }
            }
            if (complete) {
                v1::MonitorRequest message;
                message.mutable_command_applied()->set_command_id(id);
                enqueue_upstream(std::move(message));
            }
        }
    }
}

void RiskClient::on_accept(const std::string& sentinel_id) {
    support::info("risk: session accepted by sentinel '{}'", sentinel_id);
    // release: pairs with the acquire in session_established(); publishes the
    // session state set up before this point to threads that observe `true`.
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
    if (!ingress_.broadcast(*decoded)) {
        support::warn("risk: command {} dropped (shutting down)", command.command_id());
    }
}

void RiskClient::on_session_done(bool was_established, const std::string& reason) {
    // release: same pairing as in on_accept(); a reader that sees `false`
    // after a session also sees that session's teardown.
    established_.store(false, std::memory_order_release);
    if (was_established) {
        support::warn("risk: session closed: {}", reason);
        if (!ingress_.broadcast(domain::RiskLinkStatus{.connected = false})) {
            support::warn("risk: could not journal link-down (shutting down)");
        }
    } else {
        support::warn("risk: risk-sentinel unavailable ({}); continuing fail-open", reason);
    }
    {
        const std::scoped_lock lock{mutex_};
        session_done_ = true;
    }
    wake_.notify_all();
}

}  // namespace lockstep::risk_client
