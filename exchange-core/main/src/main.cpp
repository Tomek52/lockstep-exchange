// Composition root: the only place that knows every concrete type. It builds
// the application core, plugs adapters into its ports, and owns the shutdown
// order (ADR-0002, ADR-0003).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <print>
#include <ranges>
#include <span>
#include <stacktrace>
#include <string>
#include <string_view>
#include <vector>

#include "lockstep/app/engine.hpp"
#include "lockstep/app/fatal.hpp"
#include "lockstep/app/ports/clock.hpp"
#include "lockstep/grpc/order_entry_service.hpp"
#include "lockstep/grpc/server.hpp"
#include "lockstep/journal/memory_journal.hpp"
#include "lockstep/risk_client/risk_client.hpp"
#include "lockstep/support/log.hpp"

#include "options.hpp"
#include <pthread.h>

namespace {

using namespace lockstep;

void print_fatal(std::string_view message) noexcept {
    // Best effort: we are about to abort, so allocation failures here are moot.
    std::println(stderr, "FATAL: {}\n{}", message, std::to_string(std::stacktrace::current(1)));
    std::fflush(stderr);
}

void install_fatal_handlers() {
    app::set_fatal_handler(&print_fatal);
    std::set_terminate([] {
        std::string what = "std::terminate called";
        if (const std::exception_ptr current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                what += std::string{" after uncaught exception: "} + e.what();
            } catch (...) {
                what += " after uncaught non-standard exception";
            }
        }
        print_fatal(what);
        std::abort();
    });
}

/// Blocks SIGINT/SIGTERM in the calling thread; threads created afterwards
/// inherit the mask, so only main() observes them (via sigwait) and no async
/// signal handler ever races with the runtime.
sigset_t block_shutdown_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    return signals;
}

int run(const main_app::Options& options) {
    const sigset_t signals = block_shutdown_signals();

    app::SystemClock clock;
    app::EngineConfig config{
        .instruments = options.instruments | std::views::transform([](std::uint32_t id) {
                           return domain::InstrumentSpec{.id = domain::InstrumentId{id}};
                       }) |
                       std::ranges::to<std::vector>(),
        .shard_count = options.shards,
    };
    // TODO(task-008): FileJournalWriter per shard. Until then nothing is durable.
    support::warn("journal: using NullJournal - commands are NOT persisted (see docs/tasks/008)");
    app::Engine engine{std::move(config),
                       [](domain::ShardId) { return std::make_unique<journal::NullJournal>(); },
                       clock};

    std::unique_ptr<risk_client::RiskClient> risk;
    if (options.risk_enabled) {
        risk = std::make_unique<risk_client::RiskClient>(
            risk_client::RiskClientConfig{
                .target = options.risk_target,
                .exchange_id = options.exchange_id,
                .shard_count = static_cast<std::uint32_t>(options.shards)},
            engine);
        engine.add_subscriber(*risk);
    }

    engine.start();
    grpc_adapter::OrderEntryService order_entry{engine};
    grpc_adapter::GrpcServer server{options.listen, {&order_entry}};
    if (risk) {
        risk->start();
    }
    support::info("exchange-core listening on port {} (shards={}, instruments={})", server.port(),
                  options.shards, options.instruments.size());

    int signal = 0;
    sigwait(&signals, &signal);
    support::info("received signal {}, shutting down", signal);

    // Shutdown protocol: stop every producer, then the engine (ADR-0003).
    server.shutdown(std::chrono::seconds{2});
    if (risk) {
        risk->stop();
    }
    engine.stop();
    support::info("exchange-core stopped cleanly");
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    install_fatal_handlers();

    const auto options = main_app::parse_options(std::span{argv, static_cast<std::size_t>(argc)});
    if (!options) {
        std::println(stderr, "error: {}\n{}", options.error(), main_app::usage);
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::println("{}", main_app::usage);
        return EXIT_SUCCESS;
    }
    try {
        return run(*options);
    } catch (const std::exception& e) {
        // Startup errors (bad address, bad config) surface here (ADR-0008).
        support::error("startup failed: {}", e.what());
        return EXIT_FAILURE;
    }
}
