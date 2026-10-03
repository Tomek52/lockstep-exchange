// Composition root: the only place that knows every concrete type. It builds
// the application core, plugs adapters into its ports, and owns the shutdown
// order (ADR-0002, ADR-0003).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
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
#include "lockstep/app/router.hpp"
#include "lockstep/domain/shard_engine.hpp"
#include "lockstep/grpc/order_entry_service.hpp"
#include "lockstep/grpc/server.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/format.hpp"
#include "lockstep/journal/record_codec.hpp"
#include "lockstep/risk_client/risk_client.hpp"
#include "lockstep/support/log.hpp"

#include "options.hpp"
#include <pthread.h>

namespace {

using namespace lockstep;

void print_fatal(std::string_view message) noexcept {
    try {
        std::println(stderr, "FATAL: {}\n{}", message, std::to_string(std::stacktrace::current(1)));
    } catch (...) {
        // Formatting failed (e.g. out of memory); still say *something*.
        (void)std::fputs("FATAL (stack trace unavailable)\n", stderr);
    }
    (void)std::fflush(stderr);
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
    // One journal file per shard (ADR-0004). The header records the shard's
    // config so replay can refuse a journal recorded under another one
    // (ADR-0012, ADR-0017). The Engine builds its own Router from the same
    // inputs; Router::round_robin is a pure function of them, so both agree.
    std::filesystem::create_directories(options.journal_dir);
    const auto router = app::Router::round_robin(config.instruments, config.shard_count);
    const auto sync =
        options.fsync_every_commit ? journal::SyncPolicy::EveryCommit : journal::SyncPolicy::None;
    // Captures by value what it needs from `config`: the Engine takes `config`
    // by move before it calls the factory.
    auto journal_factory =
        [&router, &options, sync, shard_count = static_cast<std::uint32_t>(config.shard_count),
         policy = config.risk_link_policy](domain::ShardId shard) -> std::unique_ptr<app::Journal> {
        const auto instruments = router.instruments_of(shard);
        const domain::ShardConfig shard_config{
            .shard = shard,
            .instruments = {instruments.begin(), instruments.end()},
            .risk_link_policy = policy};
        return journal::FileJournalWriter::create(
            options.journal_dir,
            journal::FileHeader{.shard = shard,
                                .shard_count = shard_count,
                                .config_hash = journal::config_hash(shard_config)},
            sync);
    };
    app::Engine engine{std::move(config), std::move(journal_factory), clock};
    support::info("journal: writing {} (fsync={})", options.journal_dir.string(),
                  options.fsync_every_commit ? "commit" : "none");

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

int main(int argc, char** argv) try {
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
    return run(*options);
} catch (const std::exception& e) {
    // Startup errors (bad address, bad config) surface here (ADR-0008).
    (void)std::fputs("startup failed: ", stderr);
    (void)std::fputs(e.what(), stderr);
    (void)std::fputs("\n", stderr);
    return EXIT_FAILURE;
} catch (...) {
    return EXIT_FAILURE;
}
