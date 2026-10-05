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
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lockstep/app/digest.hpp"
#include "lockstep/app/engine.hpp"
#include "lockstep/app/fatal.hpp"
#include "lockstep/app/ports/clock.hpp"
#include "lockstep/app/router.hpp"
#include "lockstep/app/shard_runtime.hpp"
#include "lockstep/config/exchange_config.hpp"
#include "lockstep/domain/shard_engine.hpp"
#include "lockstep/grpc/order_entry_service.hpp"
#include "lockstep/grpc/server.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/format.hpp"
#include "lockstep/journal/record_codec.hpp"
#include "lockstep/journal/restart.hpp"
#include "lockstep/risk_client/risk_client.hpp"
#include "lockstep/support/log.hpp"

#include "options.hpp"
#include "shard_digest.hpp"
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

/// Builds the engine configuration from either the JSON config file (ADR-0019)
/// or, when none is given, the individual flags with their defaults. The two
/// are mutually exclusive, enforced in parse_options.
std::expected<app::EngineConfig, std::string> build_engine_config(
    const main_app::Options& options) {
    if (!options.config_file.empty()) {
        auto config = config::load_config(options.config_file);
        if (!config) {
            return std::unexpected(std::move(config).error());
        }
        return app::EngineConfig{.instruments = std::move(config->instruments),
                                 .shard_count = config->shards,
                                 .risk_link_policy = config->risk_link_policy};
    }
    return app::EngineConfig{
        .instruments = options.instruments | std::views::transform([](std::uint32_t id) {
                           return domain::InstrumentSpec{.id = domain::InstrumentId{id}};
                       }) |
                       std::ranges::to<std::vector>(),
        .shard_count = options.shards,
    };
}

int run(const main_app::Options& options) {
    const sigset_t signals = block_shutdown_signals();

    app::SystemClock clock;
    auto engine_config = build_engine_config(options);
    if (!engine_config) {
        // Startup error (bad config file); surfaced like any other (ADR-0008).
        throw std::runtime_error("config: " + engine_config.error());
    }
    app::EngineConfig config{std::move(*engine_config)};
    // One journal file per shard (ADR-0004). The header records the shard's
    // config so replay can refuse a journal recorded under another one
    // (ADR-0012, ADR-0017). The Engine builds its own Router from the same
    // inputs; Router::round_robin is a pure function of them, so both agree.
    const bool journal_dir_created = std::filesystem::create_directories(options.journal_dir);
    // A freshly created directory's entry in its parent is not durable until
    // that parent is fsynced (fdatasync on a file inside it only covers the
    // file's contents). Only EveryCommit promises crash durability, so only
    // that policy needs to pay for it.
    if (journal_dir_created && options.fsync_every_commit) {
        const auto parent = options.journal_dir.parent_path();
        const std::filesystem::path dir_to_sync =
            parent.empty() ? std::filesystem::current_path() : parent;
        if (const int error = journal::sync_directory(dir_to_sync); error != 0) {
            throw std::runtime_error("journal: cannot fsync directory '" + dir_to_sync.string() +
                                     "': " + std::generic_category().message(error));
        }
    }
    const auto router = app::Router::round_robin(config.instruments, config.shard_count);
    const auto sync =
        options.fsync_every_commit ? journal::SyncPolicy::EveryCommit : journal::SyncPolicy::None;

    // Restart (ADR-0020): for each shard whose journal file already exists
    // (e.g. after `docker compose stop` and `start`), recover its tail and
    // read it back before this run touches the file at all. A refusal
    // (corrupt, torn beyond repair, or recorded under a different config)
    // stops startup naming the file; recover_for_restart never deletes or
    // truncates a file it refuses. A shard with no existing file gets an
    // empty, "fresh start" result.
    std::vector<journal::ShardRestart> shard_restarts;
    shard_restarts.reserve(config.shard_count);
    for (std::uint32_t s = 0; s < config.shard_count; ++s) {
        const domain::ShardId shard{s};
        const auto instruments = router.instruments_of(shard);
        const domain::ShardConfig shard_config{
            .shard = shard,
            .instruments = {instruments.begin(), instruments.end()},
            .risk_link_policy = config.risk_link_policy};
        auto recovered = journal::recover_for_restart(
            options.journal_dir, shard_config, static_cast<std::uint32_t>(config.shard_count));
        if (!recovered) {
            throw std::runtime_error(std::move(recovered).error());
        }
        shard_restarts.push_back(std::move(*recovered));
    }

    // Captures by value what it needs from `config`: the Engine takes `config`
    // by move before it calls the factory.
    auto journal_factory =
        [&router, &options, &shard_restarts, sync,
         shard_count = static_cast<std::uint32_t>(config.shard_count),
         policy = config.risk_link_policy](domain::ShardId shard) -> std::unique_ptr<app::Journal> {
        if (shard_restarts.at(shard.value()).existed) {
            // Already recovered and header-checked above: reopen it to
            // append after the records resume_factory is about to replay
            // (ADR-0020), rather than refuse it like create()'s O_EXCL would.
            return journal::FileJournalWriter::open_for_append(options.journal_dir, shard, sync);
        }
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
    // Rebuilds each shard's state from what was just recovered (ADR-0020),
    // before its thread starts. Moves each shard's commands out exactly
    // once: Engine's constructor calls this once per shard.
    app::ResumeFactory resume_factory = [&shard_restarts](domain::ShardId shard) {
        return std::move(shard_restarts.at(shard.value()).resume_commands);
    };
    // Captured before the Engine moves `config` out from under us, for the
    // startup log line and for --print-digest-on-exit after shutdown (the
    // config file may differ from the --shards flag).
    const std::size_t shard_count_for_log = config.shard_count;
    const std::size_t instrument_count_for_log = config.instruments.size();
    app::Engine engine{std::move(config), std::move(journal_factory), clock,
                       std::move(resume_factory)};
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
                  shard_count_for_log, instrument_count_for_log);

    int signal = 0;
    sigwait(&signals, &signal);
    support::info("received signal {}, shutting down", signal);

    // Shutdown protocol: stop every producer, then the engine (ADR-0003).
    server.shutdown(std::chrono::seconds{2});
    if (risk) {
        risk->stop();
    }
    engine.stop();

    if (options.print_digest_on_exit) {
        // Computed from this run's own live output (ShardRuntime's
        // DigestBuilder, folded by each shard thread as it produced events
        // and replies - including, first, whatever it resumed from an
        // existing journal at startup), never by re-reading the journal
        // back from disk: re-reading would make this check a
        // replay-equals-replay tautology that could not catch a bug in how,
        // or whether, the live run itself folds its own output into the
        // digest (task 010 review F1). lockstep-replay is the from-disk
        // tool; this flag is the live-run oracle it is compared against.
        //
        // Book snapshots are taken here, after engine.stop() above joined
        // every shard thread, so no thread is still touching the engine
        // (digest_builder()'s own comment states the happens-before this
        // relies on).
        for (std::uint32_t s = 0; s < shard_count_for_log; ++s) {
            const domain::ShardId shard{s};
            const app::ShardRuntime& shard_runtime = engine.shard(shard);
            const app::DigestBuilder& builder = shard_runtime.digest_builder();
            std::vector<domain::BookSnapshot> books;
            for (const domain::InstrumentSpec& spec : router.instruments_of(shard)) {
                if (const domain::OrderBook* book = shard_runtime.engine().book(spec.id)) {
                    books.push_back(book->snapshot());
                }
            }
            std::println("{}", main_app::format_shard_digest_line(
                                   main_app::ShardDigestLine{.shard = shard,
                                                             .commands = builder.commands(),
                                                             .events = builder.events(),
                                                             .digest = builder.finish(books)}));
        }
    }

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
