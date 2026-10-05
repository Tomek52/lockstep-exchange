#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <thread>
#include <vector>

#include "lockstep/app/ports/clock.hpp"
#include "lockstep/app/ports/command_ingress.hpp"
#include "lockstep/app/ports/event_subscriber.hpp"
#include "lockstep/app/ports/journal.hpp"
#include "lockstep/app/publisher.hpp"
#include "lockstep/app/router.hpp"
#include "lockstep/app/shard_runtime.hpp"
#include "lockstep/concurrency/cache_aligned.hpp"
#include "lockstep/concurrency/idle_strategy.hpp"
#include "lockstep/domain/risk_state.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::app {

struct EngineConfig {
    std::vector<domain::InstrumentSpec> instruments;
    std::size_t shard_count{1};
    std::size_t ingress_capacity{4096};
    std::size_t egress_capacity{16384};
    domain::RiskLinkPolicy risk_link_policy{domain::RiskLinkPolicy::FailOpen};
};

/// Application core facade: N shard threads + 1 publisher thread, exposed to
/// driving adapters through the CommandIngress port.
///
/// Lifecycle: construct -> add_subscriber()* -> start() -> ... -> stop().
/// Shutdown protocol (ADR-0003): the owner stops every producer first (gRPC
/// server, risk client), then calls stop(), which stops and drains the shards,
/// then stops and drains the publisher, so every accepted command gets its
/// completion invoked.
class Engine final : public CommandIngress {
public:
    /// `resume_factory`, if set, is called once per shard right after its
    /// journal and before any thread starts, to rebuild state recovered from
    /// an existing journal (ADR-0020). Left default-constructed ("falsy") for
    /// a fresh exchange with nothing to resume.
    Engine(EngineConfig config,
           JournalFactory journal_factory,
           Clock& clock,
           ResumeFactory resume_factory = nullptr);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine() override;

    void add_subscriber(EventSubscriber& subscriber);
    /// Registers a new Subscription at runtime, blocking until registration
    /// has taken effect - usually within one publisher iteration, but can
    /// take longer (see Publisher::subscribe's doc for the full contract
    /// and why). Callable from any thread, before or after start(), except
    /// the publisher thread itself (e.g. from inside a completion or an
    /// on_ready() hook): that case aborts via fatal() rather than
    /// deadlocking.
    std::shared_ptr<Subscription> subscribe(SubscriptionFilter filter, std::size_t capacity);
    void start();
    void stop();

    [[nodiscard]] std::expected<void, SubmitError> submit(domain::Command command,
                                                          Completion completion) override;
    [[nodiscard]] std::expected<void, SubmitError> broadcast(domain::Command command) override;

    [[nodiscard]] const Router& router() const noexcept { return router_; }
    /// Only valid while stopped (tests compare engine state after replay).
    [[nodiscard]] const ShardRuntime& shard(domain::ShardId shard) const;

    /// Every shard's runtime counters, in shard-id order. Safe to call from
    /// any thread at any time, including while the engine is running; see
    /// ShardStats's comment.
    [[nodiscard]] std::vector<ShardStats> shard_stats() const;

private:
    // Declared before shards_/publisher_: shared by every shard (rung after
    // a batch is released to egress) and the publisher (parks on it), and
    // constructing it needs neither - avoids an initialization-order cycle
    // (Publisher needs the shards' egress queues, so it cannot be built
    // before shards_; shards need a reference to the publisher's doorbell,
    // so the doorbell cannot live inside Publisher itself).
    //
    // alignas on both this and router_: every shard thread rings this
    // doorbell, so without separating it from router_ (and the vtable
    // pointer ahead of it) those cross-thread writes would false-share a
    // cache line with state the construction-time-only Router and read-mostly
    // vptr otherwise wouldn't need to bounce (ADR-0011's cache-line rule).
    alignas(concurrency::cache_line_size) concurrency::Doorbell publisher_doorbell_;
    alignas(concurrency::cache_line_size) Router router_;
    std::vector<std::unique_ptr<ShardRuntime>> shards_;
    Publisher publisher_;
    std::vector<std::jthread> shard_threads_;
    std::jthread publisher_thread_;
    std::atomic<bool> accepting_{false};
    // Distinguishes "stop() before any start()" from "stop() after an
    // earlier start()+stop() cycle" - publisher_thread_.joinable() is false
    // in both cases (join() leaves a jthread non-joinable too), so stop()
    // needs this to decide whether to call Publisher::close_before_start().
    // Plain bool: start()/stop() are an owner-thread lifecycle (class
    // comment), never called concurrently with each other.
    bool start_called_{false};
};

}  // namespace lockstep::app
