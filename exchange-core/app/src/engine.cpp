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
                                                       Clock& clock) {
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
        shards.push_back(
            std::make_unique<ShardRuntime>(std::move(shard_config), journal_factory(id), clock));
    }
    return shards;
}

}  // namespace

Engine::Engine(EngineConfig config, JournalFactory journal_factory, Clock& clock)
    : router_{Router::round_robin(config.instruments, config.shard_count)},
      shards_{make_shards(config, router_, journal_factory, clock)},
      publisher_{egress_queues(shards_)} {}

Engine::~Engine() {
    stop();
}

void Engine::add_subscriber(EventSubscriber& subscriber) {
    publisher_.add_subscriber(subscriber);
}

void Engine::start() {
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
    if (!shards_[shard->value()]->ingress().try_push(
            InboundCommand{command, std::move(completion)})) {
        return std::unexpected(SubmitError::Overloaded);
    }
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
    }
    return {};
}

const ShardRuntime& Engine::shard(domain::ShardId shard) const {
    return *shards_.at(shard.value());
}

}  // namespace lockstep::app
