// In-process gRPC test of the RiskClient adapter against a fake risk-sentinel
// (a RiskSentinelService::CallbackService on a loopback port) and a fake
// CommandIngress that records broadcasts. Covers the risk loop end to end
// without the real sentinel (task 014, ADR-0013).
#include "lockstep/risk_client/risk_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/ports/command_ingress.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/events.hpp"
#include <lockstep/v1/risk.grpc.pb.h>

namespace lockstep::risk_client {
namespace {

using namespace std::chrono_literals;

/// Records risk-command broadcasts (link status and decoded commands). The
/// RiskClient calls broadcast() from gRPC callback threads, so guard with a
/// mutex and a condition variable the test can wait on.
class RecordingIngress final : public app::CommandIngress {
public:
    std::expected<void, app::SubmitError> submit(domain::Command /*command*/,
                                                 app::Completion /*completion*/) override {
        ADD_FAILURE() << "risk client must not submit(); it broadcasts";
        return std::unexpected(app::SubmitError::NotRoutable);
    }

    std::expected<void, app::SubmitError> broadcast(domain::Command command) override {
        {
            const std::scoped_lock lock{mutex_};
            commands_.push_back(command);
        }
        cv_.notify_all();
        return {};
    }

    template <typename Predicate>
    bool wait_for(Predicate pred, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock{mutex_};
        return cv_.wait_for(lock, timeout, [&] { return pred(commands_); });
    }

    std::vector<domain::Command> snapshot() {
        const std::scoped_lock lock{mutex_};
        return commands_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<domain::Command> commands_;
};

/// An ingress whose broadcast() blocks until released, like Engine::broadcast()
/// spinning on full shard queues (ADR-0013). Records what got through.
class BlockingIngress final : public app::CommandIngress {
public:
    std::expected<void, app::SubmitError> submit(domain::Command /*command*/,
                                                 app::Completion /*completion*/) override {
        ADD_FAILURE() << "risk client must not submit(); it broadcasts";
        return std::unexpected(app::SubmitError::NotRoutable);
    }

    std::expected<void, app::SubmitError> broadcast(domain::Command command) override {
        std::unique_lock lock{mutex_};
        ++entered_;
        cv_.notify_all();
        cv_.wait(lock, [&] { return open_; });
        commands_.push_back(command);
        cv_.notify_all();
        return {};
    }

    void release() {
        {
            const std::scoped_lock lock{mutex_};
            open_ = true;
        }
        cv_.notify_all();
    }

    bool wait_entered(int count, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock{mutex_};
        return cv_.wait_for(lock, timeout, [&] { return entered_ >= count; });
    }

    template <typename Predicate>
    bool wait_for(Predicate pred, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock{mutex_};
        return cv_.wait_for(lock, timeout, [&] { return pred(commands_); });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int entered_{0};
    bool open_{false};
    std::vector<domain::Command> commands_;
};

/// Shared record of what the fake sentinel received upstream, plus a handle to
/// push one command downstream. Survives reconnects (keyed by nothing - the
/// test drives it directly).
struct SentinelState {
    std::mutex mutex;
    std::condition_variable cv;
    int hellos{0};
    std::uint32_t last_hello_shard_count{0};
    std::string last_hello_exchange_id;
    std::vector<v1::ExecutionReport> executions;
    std::vector<std::uint64_t> command_applied_ids;
    int sessions{0};
    int heartbeats{0};

    // A command the active reactor should push downstream once, if set.
    std::atomic<bool> have_pending_command{false};
    v1::RiskCommand pending_command;

    template <typename Predicate>
    bool wait_for(Predicate pred, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, timeout, [&] { return pred(*this); });
    }
};

/// One server-side Monitor stream. Accepts immediately, then echoes nothing
/// but records every upstream message; pushes a pending command if the test
/// queued one.
class FakeMonitorReactor final
    : public grpc::ServerBidiReactor<v1::MonitorRequest, v1::MonitorResponse> {
public:
    explicit FakeMonitorReactor(SentinelState& state) : state_{state} {
        {
            const std::scoped_lock lock{state_.mutex};
            ++state_.sessions;
        }
        state_.cv.notify_all();

        accept_.mutable_accept()->mutable_protocol()->set_major(1);
        accept_.mutable_accept()->set_sentinel_id("fake-sentinel");
        StartWrite(&accept_);
        StartRead(&request_);

        if (state_.have_pending_command.exchange(false)) {
            command_ = state_.pending_command;
            // Deferred until the accept write finishes (one write at a time).
            command_pending_ = true;
        }
    }

    void OnWriteDone(bool ok) override {
        if (!ok) {
            if (!finishing_.exchange(true)) {
                Finish(grpc::Status::OK);
            }
            return;
        }
        if (command_pending_) {
            command_pending_ = false;
            *response_.mutable_command() = command_;
            StartWrite(&response_);
        }
    }

    void OnReadDone(bool ok) override {
        if (!ok) {
            // Client half-closed or the stream was cancelled: end the RPC so
            // the server can drain on shutdown.
            if (!finishing_.exchange(true)) {
                Finish(grpc::Status::OK);
            }
            return;
        }
        {
            const std::scoped_lock lock{state_.mutex};
            switch (request_.message_case()) {
                case v1::MonitorRequest::kHello:
                    ++state_.hellos;
                    state_.last_hello_shard_count = request_.hello().shard_count();
                    state_.last_hello_exchange_id = request_.hello().exchange_id();
                    break;
                case v1::MonitorRequest::kExecution:
                    state_.executions.push_back(request_.execution());
                    break;
                case v1::MonitorRequest::kCommandApplied:
                    state_.command_applied_ids.push_back(request_.command_applied().command_id());
                    break;
                case v1::MonitorRequest::kHeartbeat:
                    ++state_.heartbeats;
                    break;
                case v1::MonitorRequest::MESSAGE_NOT_SET:
                    break;
            }
        }
        state_.cv.notify_all();
        StartRead(&request_);
    }

    void OnDone() override { delete this; }

private:
    SentinelState& state_;
    v1::MonitorResponse accept_;
    v1::MonitorResponse response_;
    v1::MonitorRequest request_;
    v1::RiskCommand command_;
    bool command_pending_{false};
    std::atomic<bool> finishing_{false};
};

class FakeSentinel final : public v1::RiskSentinelService::CallbackService {
public:
    explicit FakeSentinel(SentinelState& state) : state_{state} {}

    grpc::ServerBidiReactor<v1::MonitorRequest, v1::MonitorResponse>* Monitor(
        grpc::CallbackServerContext* /*context*/) override {
        return new FakeMonitorReactor(state_);
    }

private:
    SentinelState& state_;
};

/// A server the test can tear down and rebuild on the same port.
class ScopedServer {
public:
    ScopedServer(int port, FakeSentinel& service) {
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:" + std::to_string(port),
                                 grpc::InsecureServerCredentials(), &bound_port_);
        builder.RegisterService(&service);
        server_ = builder.BuildAndStart();
    }
    ~ScopedServer() {
        if (server_) {
            server_->Shutdown(std::chrono::system_clock::now() + 100ms);
            server_->Wait();
        }
    }
    ScopedServer(const ScopedServer&) = delete;
    ScopedServer& operator=(const ScopedServer&) = delete;
    ScopedServer(ScopedServer&&) = delete;
    ScopedServer& operator=(ScopedServer&&) = delete;

    [[nodiscard]] int port() const noexcept { return bound_port_; }

private:
    int bound_port_{0};
    std::unique_ptr<grpc::Server> server_;
};

RiskClientConfig fast_config(int port, std::uint32_t shard_count) {
    return RiskClientConfig{.target = "127.0.0.1:" + std::to_string(port),
                            .exchange_id = "exch-1",
                            .shard_count = shard_count,
                            .heartbeat_interval = 100ms,
                            .receive_timeout = 5000ms,
                            .backoff_base = 50ms,
                            .backoff_max = 400ms,
                            .max_queued_messages = 100'000};
}

app::PublishedEvent trade_event() {
    const domain::Trade trade{.instrument = domain::InstrumentId{7},
                              .price = domain::Price{101},
                              .quantity = domain::Quantity{5},
                              .aggressor_side = domain::Side::Buy,
                              .maker_order = domain::OrderId{10},
                              .maker_trader = domain::TraderId{100},
                              .taker_order = domain::OrderId{20},
                              .taker_trader = domain::TraderId{200}};
    return app::PublishedEvent{.shard = domain::ShardId{0},
                               .sequence = domain::SequenceNumber{1},
                               .timestamp = domain::Timestamp{123},
                               .event = trade};
}

app::PublishedEvent applied_event(std::uint64_t command_id, std::uint32_t shard) {
    return app::PublishedEvent{
        .shard = domain::ShardId{shard},
        .sequence = domain::SequenceNumber{1},
        .timestamp = domain::Timestamp{123},
        .event = domain::RiskCommandApplied{domain::RiskCommandId{command_id}}};
}

TEST(RiskClient, HandshakeSendsHelloWithExchangeIdAndShardCount) {
    SentinelState state;
    FakeSentinel service{state};
    ScopedServer server{0, service};

    RecordingIngress ingress;
    RiskClient client{fast_config(server.port(), 3), ingress};
    client.start();

    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.hellos >= 1; }));
    {
        const std::scoped_lock lock{state.mutex};
        EXPECT_EQ(state.last_hello_exchange_id, "exch-1");
        EXPECT_EQ(state.last_hello_shard_count, 3U);
    }
    client.stop();
}

TEST(RiskClient, TradeEventProducesTwoExecutionReports) {
    SentinelState state;
    FakeSentinel service{state};
    ScopedServer server{0, service};

    RecordingIngress ingress;
    RiskClient client{fast_config(server.port(), 1), ingress};
    client.start();
    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.hellos >= 1; }));

    const auto event = trade_event();
    client.on_events(std::span{&event, 1});

    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.executions.size() >= 2; }));
    const std::scoped_lock lock{state.mutex};
    ASSERT_EQ(state.executions.size(), 2U);
    EXPECT_FALSE(state.executions[0].is_maker());
    EXPECT_EQ(state.executions[0].trader_id(), 200U);
    EXPECT_EQ(state.executions[0].side(), v1::SIDE_BUY);
    EXPECT_TRUE(state.executions[1].is_maker());
    EXPECT_EQ(state.executions[1].trader_id(), 100U);
    EXPECT_EQ(state.executions[1].side(), v1::SIDE_SELL);
    client.stop();
}

TEST(RiskClient, CommandIsBroadcastAndAckedOnceAllShardsApply) {
    SentinelState state;
    state.pending_command.set_command_id(55);
    state.pending_command.mutable_kill_switch()->set_engaged(true);
    state.have_pending_command.store(true);

    FakeSentinel service{state};
    ScopedServer server{0, service};

    RecordingIngress ingress;
    constexpr std::uint32_t shards = 3;
    RiskClient client{fast_config(server.port(), shards), ingress};
    client.start();

    // The pushed RiskCommand becomes one broadcast (plus the link-up status).
    ASSERT_TRUE(ingress.wait_for([](const std::vector<domain::Command>& cmds) {
        for (const auto& c : cmds) {
            if (const auto* kill = std::get_if<domain::KillSwitch>(&c)) {
                return kill->command_id == domain::RiskCommandId{55};
            }
        }
        return false;
    }));

    // Feeding shard_count RiskCommandApplied events yields exactly one ack.
    for (std::uint32_t shard = 0; shard < shards; ++shard) {
        const auto event = applied_event(55, shard);
        client.on_events(std::span{&event, 1});
    }

    // Upstream messages are FIFO, so once the ack for a later marker command
    // (56) has arrived, any erroneous extra ack for 55 would already be there:
    // wait on that instead of sleeping.
    for (std::uint32_t shard = 0; shard < shards; ++shard) {
        const auto event = applied_event(56, shard);
        client.on_events(std::span{&event, 1});
    }
    ASSERT_TRUE(
        state.wait_for([](const SentinelState& s) { return s.command_applied_ids.size() >= 2; }));
    {
        const std::scoped_lock lock{state.mutex};
        ASSERT_EQ(state.command_applied_ids.size(), 2U);
        EXPECT_EQ(state.command_applied_ids[0], 55U);
        EXPECT_EQ(state.command_applied_ids[1], 56U);
    }
    client.stop();
}

TEST(RiskClient, DuplicateApplyFromOneShardDoesNotAckEarly) {
    SentinelState state;
    FakeSentinel service{state};
    ScopedServer server{0, service};

    RecordingIngress ingress;
    RiskClient client{fast_config(server.port(), 2), ingress};
    client.start();
    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.hellos >= 1; }));

    // Shard 0 applies command 70 twice (e.g. a resend, ADR-0013): shard 1 has
    // not applied it, so there must be no ack yet. Then shard 1 applies it and
    // the first ack is sent; shard 1's second apply completes the second round.
    for (const std::uint32_t shard : {0U, 0U}) {
        const auto event = applied_event(70, shard);
        client.on_events(std::span{&event, 1});
    }
    // FIFO marker: command 71 fully applied. Its ack must be the first one.
    for (std::uint32_t shard = 0; shard < 2; ++shard) {
        const auto event = applied_event(71, shard);
        client.on_events(std::span{&event, 1});
    }
    ASSERT_TRUE(
        state.wait_for([](const SentinelState& s) { return !s.command_applied_ids.empty(); }));
    {
        const std::scoped_lock lock{state.mutex};
        ASSERT_EQ(state.command_applied_ids.size(), 1U);
        EXPECT_EQ(state.command_applied_ids[0], 71U);
    }

    for (const std::uint32_t shard : {1U, 1U}) {
        const auto event = applied_event(70, shard);
        client.on_events(std::span{&event, 1});
    }
    ASSERT_TRUE(
        state.wait_for([](const SentinelState& s) { return s.command_applied_ids.size() >= 3; }));
    {
        const std::scoped_lock lock{state.mutex};
        ASSERT_EQ(state.command_applied_ids.size(), 3U);
        EXPECT_EQ(state.command_applied_ids[1], 70U);
        EXPECT_EQ(state.command_applied_ids[2], 70U);
    }
    client.stop();
}

TEST(RiskClient, ReconnectsAfterServerRestartWithExactlyOneLinkTransitionEach) {
    SentinelState state;
    FakeSentinel service{state};

    RecordingIngress ingress;
    int port = 0;
    auto server = std::make_unique<ScopedServer>(0, service);
    port = server->port();

    RiskClient client{fast_config(port, 1), ingress};
    client.start();

    // First session up: link status connected = true.
    ASSERT_TRUE(
        ingress.wait_for([](const std::vector<domain::Command>& cmds) { return !cmds.empty(); }));
    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.sessions >= 1; }));

    // Kill the server; the client should see the link drop.
    server.reset();
    ASSERT_TRUE(ingress.wait_for([](const std::vector<domain::Command>& cmds) {
        for (const auto& c : cmds) {
            if (const auto* link = std::get_if<domain::RiskLinkStatus>(&c)) {
                if (!link->connected) {
                    return true;
                }
            }
        }
        return false;
    }));

    // Restart on the same port; the client reconnects within the back-off.
    server = std::make_unique<ScopedServer>(port, service);
    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.sessions >= 2; }, 4000ms));
    ASSERT_TRUE(ingress.wait_for(
        [](const std::vector<domain::Command>& cmds) {
            // After the drop, a second connected = true must appear.
            int connected = 0;
            for (const auto& c : cmds) {
                if (const auto* link = std::get_if<domain::RiskLinkStatus>(&c)) {
                    if (link->connected) {
                        ++connected;
                    }
                }
            }
            return connected >= 2;
        },
        4000ms));

    // Exactly one of each transition in order: up, down, up.
    const auto cmds = ingress.snapshot();
    std::vector<bool> transitions;
    for (const auto& c : cmds) {
        if (const auto* link = std::get_if<domain::RiskLinkStatus>(&c)) {
            transitions.push_back(link->connected);
        }
    }
    ASSERT_GE(transitions.size(), 3U);
    EXPECT_TRUE(transitions[0]);
    EXPECT_FALSE(transitions[1]);
    EXPECT_TRUE(transitions[2]);

    client.stop();
    server.reset();
}

TEST(RiskClient, StalledIngressDoesNotStallHeartbeatsOrReconnect) {
    // The shards' ingress is full: broadcast() blocks. The gRPC callbacks and
    // the manager must not be held up behind it - heartbeats keep flowing and
    // a dropped stream is reconnected - and once the ingress frees up the
    // link transitions arrive in order: up, down, up.
    SentinelState state;
    FakeSentinel service{state};
    auto server = std::make_unique<ScopedServer>(0, service);
    const int port = server->port();

    BlockingIngress ingress;
    RiskClient client{fast_config(port, 1), ingress};
    client.start();

    ASSERT_TRUE(ingress.wait_entered(1));  // link-up is stuck in broadcast()
    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.heartbeats >= 3; }, 3000ms));

    server.reset();  // the stream drops; OnDone must not wait on the ingress
    server = std::make_unique<ScopedServer>(port, service);
    ASSERT_TRUE(state.wait_for([](const SentinelState& s) { return s.sessions >= 2; }, 4000ms));

    ingress.release();
    ASSERT_TRUE(ingress.wait_for(
        [](const std::vector<domain::Command>& cmds) { return cmds.size() >= 3; }));
    client.stop();

    std::vector<bool> transitions;
    ASSERT_TRUE(ingress.wait_for([&](const std::vector<domain::Command>& cmds) {
        for (const auto& c : cmds) {
            if (const auto* link = std::get_if<domain::RiskLinkStatus>(&c)) {
                transitions.push_back(link->connected);
            }
        }
        return true;
    }));
    ASSERT_GE(transitions.size(), 3U);
    EXPECT_TRUE(transitions[0]);
    EXPECT_FALSE(transitions[1]);
    EXPECT_TRUE(transitions[2]);
}

TEST(RiskClient, StopDuringBackoffReturnsPromptly) {
    // No server at all: the client stays in connect/back-off. stop() must still
    // return quickly (well within the 200 ms bound).
    RecordingIngress ingress;
    // A port that was just free: bind port 0, note it, release it.
    int dead_port = 0;
    {
        SentinelState unused_state;
        FakeSentinel unused_service{unused_state};
        const ScopedServer probe{0, unused_service};
        dead_port = probe.port();
    }
    RiskClient client{fast_config(dead_port, 1), ingress};
    client.start();
    std::this_thread::sleep_for(150ms);  // let it enter back-off at least once

    const auto before = std::chrono::steady_clock::now();
    client.stop();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    EXPECT_LT(elapsed, 200ms);
}

}  // namespace
}  // namespace lockstep::risk_client
