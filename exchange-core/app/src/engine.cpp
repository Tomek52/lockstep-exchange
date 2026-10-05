#include "lockstep/app/engine.hpp"

#include <ranges>
#include <stdexcept>
#include <utility>

namespace lockstep::app {

namespace {

std::vector<EgressQueue*> egress_queues(const std::vector<std::unique_ptr<ShardRuntime>>& shards) {
    return shards | std::views::transform([](const auto& shard) { return &shard->egress(); }) |
           std::ranges::to<std::vector>();
}

std::vector<std::unique_ptr<ShardRuntime>> make_shards(const EngineConfig& config,
                                                       const Router& router,
                                                       JournalFactory& journal_factory,
                                                       ResumeFactory& resume_factory,
                                                       Clock& clock,
                                                       concurrency::Doorbell& publisher_doorbell) {
    if (config.shard_count == 0 || config.shard_count > (1U << 16U)) {
        // Startup-time configuration error: exceptions are fine here (ADR-0008).
        throw std::invalid_argument("shard_count must be in [1, 65536]");
    }
    std::vector<std::unique_ptr<ShardRuntime>> shards;
    for (std::uint32_t i = 0; i < config.shard_count; ++i) {
        const domain::ShardId id{i};
        const auto instruments = router.instruments_of(id);
        ShardRuntime::Config shard_config{
            .shard = {.shard = id,
                      .instruments = {instruments.begin(), instruments.end()},
                      .risk_link_policy = config.risk_link_policy},
            .ingress_capacity = config.ingress_capacity,
            .egress_capacity = config.egress_capacity,
        };
        auto& shard = shards.emplace_back(std::make_unique<ShardRuntime>(
            std::move(shard_config), journal_factory(id), clock, publisher_doorbell));
        // Rebuild state recovered from an existing journal before anyone can
        // observe this shard (ADR-0020): no thread has started yet, and the
        // owner thread building the Engine is the only one touching it.
        if (resume_factory) {
            shard->resume_from_journal(resume_factory(id));
        }
    }
    return shards;
}

}  // namespace

Engine::Engine(EngineConfig config, JournalFactory journal_factory, Clock& clock,
              ResumeFactory resume_factory)
    : router_{Router::round_robin(config.instruments, config.shard_count)},
      shards_{make_shards(config, router_, journal_factory, resume_factory, clock,
                          publisher_doorbell_)},
      publisher_{egress_queues(shards_), publisher_doorbell_} {}

Engine::~Engine() {
    stop();
}

void Engine::add_subscriber(EventSubscriber& subscriber) {
    publisher_.add_subscriber(subscriber);
}

std::shared_ptr<Subscription> Engine::subscribe(SubscriptionFilter filter, std::size_t capacity) {
    return publisher_.subscribe(std::move(filter), capacity);
}

void Engine::start() {
    start_called_ = true;
    for (const auto& shard : shards_) {
        shard_threads_.emplace_back(
            [runtime = shard.get()](const std::stop_token& stop) { runtime->run(stop); });
    }
    publisher_thread_ = std::jthread([this](const std::stop_token& stop) { publisher_.run(stop); });
    // relaxed: the flag only gates admission. Queue contents are synchronised
    // by the queues themselves, and thread start-up already happens-before any
    // work the threads do.
    accepting_.store(true, std::memory_order_relaxed);
}

void Engine::stop() {
    // relaxed: see start(). Producers are required to be stopped before stop()
    // (shutdown protocol in engine.hpp), so this flag is a defensive guard, not
    // the mechanism that makes shutdown lossless.
    accepting_.store(false, std::memory_order_relaxed);
    for (auto& thread : shard_threads_) {
        thread.request_stop();
    }
    shard_threads_.clear();  // joins: every shard has drained its ingress
    if (publisher_thread_.joinable()) {
        publisher_thread_.request_stop();
        publisher_thread_.join();  // publisher has drained every egress
    } else if (!start_called_) {
        // start() never ran, so there is no publisher thread to drive its
        // own shutdown - close whatever subscribe() registered through the
        // pre-start path directly (Publisher::subscribe's doc).
        publisher_.close_before_start();
    }
}

std::expected<void, SubmitError> Engine::submit(domain::Command command, Completion completion) {
    if (!accepting_.load(std::memory_order_relaxed)) {  // relaxed: see start()
        return std::unexpected(SubmitError::ShuttingDown);
    }
    const auto instrument = instrument_of(command);
    if (!instrument) {
        return std::unexpected(SubmitError::NotRoutable);
    }
    const auto shard = router_.shard_for(*instrument);
    if (!shard) {
        return std::unexpected(SubmitError::UnknownInstrument);
    }
    ShardRuntime& target = *shards_[shard->value()];
    if (!target.ingress().try_push(InboundCommand{command, std::move(completion)})) {
        return std::unexpected(SubmitError::Overloaded);
    }
    // Ring only after the push is visible, per Doorbell's happens-before
    // contract (idle_strategy.hpp) - a ring before the push would let a
    // waiter wake, see nothing, and park again with nothing left to wake it.
    target.doorbell().ring();
    return {};
}

std::expected<void, SubmitError> Engine::broadcast(domain::Command command) {
    for (const auto& shard : shards_) {
        InboundCommand inbound{command, {}};
        while (!shard->ingress().try_push(std::move(inbound))) {  // NOLINT(bugprone-use-after-move)
            if (!accepting_.load(std::memory_order_relaxed)) {    // relaxed: see start()
                return std::unexpected(SubmitError::ShuttingDown);
            }
            std::this_thread::yield();
        }
        shard->doorbell().ring();  // see submit()'s comment
    }
    return {};
}

const ShardRuntime& Engine::shard(domain::ShardId shard) const {
    return *shards_.at(shard.value());
}

std::vector<ShardStats> Engine::shard_stats() const {
    return shards_ | std::views::transform([](const auto& shard) { return shard->stats(); }) |
           std::ranges::to<std::vector>();
}

}  // namespace lockstep::app
