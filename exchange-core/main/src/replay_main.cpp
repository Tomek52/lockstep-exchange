// lockstep-replay: replays on-disk shard journals and prints a verifiable
// digest of every shard's outputs and final book state (task 010).
//
//   lockstep-replay --journal-dir=DIR --instruments=1,2,3,4 --shards=2
//   lockstep-replay --journal-dir=DIR --config=config/exchange.json
//
// Uses the same instrument -> shard assignment as exchange-core
// (app::Router::round_robin) and, with --config, the same config file
// format (ADR-0019), so a dir produced by either invocation of exchange-core
// replays identically here.
//
// Exit status: 0 when every shard replayed cleanly (a torn tail at the very
// end is reported, not an error: ADR-0012's "stop there" contract), 1 if any
// shard's journal could not be read (missing, corrupt, version or config
// mismatch), 2 on a usage error.
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lockstep/app/engine.hpp"
#include "lockstep/app/router.hpp"
#include "lockstep/domain/risk_state.hpp"
#include "lockstep/domain/types.hpp"

#include "engine_config.hpp"
#include "options.hpp"
#include "shard_digest.hpp"

namespace {

using namespace lockstep;

constexpr int exit_journal_error = 1;
constexpr int exit_usage = 2;

struct ReplayOptions {
    std::filesystem::path journal_dir;
    std::vector<std::uint32_t> instruments{1, 2, 3, 4};
    std::size_t shards{2};
    std::filesystem::path config_file;
    bool help{false};
};

constexpr const char* usage = R"(usage: lockstep-replay --journal-dir=DIR [options]
  --journal-dir=DIR            directory of shard-<id>.jnl files (required)
  --instruments=ID,ID,...      instrument ids to list               (default 1,2,3,4)
  --shards=N                   number of shards                     (default 2)
  --config=FILE                JSON config of instruments/shards    (excludes --instruments/--shards)
  --help)";

std::expected<ReplayOptions, std::string> parse_options(std::span<char* const> args) {
    ReplayOptions options;
    bool instruments_given = false;
    bool shards_given = false;
    bool config_given = false;
    bool journal_dir_given = false;
    for (const std::string_view arg : args | std::views::drop(1)) {
        const auto eq = arg.find('=');
        const std::string_view key = arg.substr(0, eq);
        const std::string_view value = eq == std::string_view::npos ? "" : arg.substr(eq + 1);

        if (key == "--help" || key == "-h") {
            options.help = true;
        } else if (key == "--journal-dir") {
            if (value.empty()) {
                return std::unexpected(std::string{"--journal-dir: a directory is required"});
            }
            options.journal_dir = value;
            journal_dir_given = true;
        } else if (key == "--instruments") {
            auto ids = main_app::parse_instruments(value);
            if (!ids) {
                return std::unexpected(ids.error());
            }
            options.instruments = std::move(*ids);
            instruments_given = true;
        } else if (key == "--shards") {
            std::size_t shards{};
            const auto [end, ec] =
                std::from_chars(value.data(), value.data() + value.size(), shards);
            if (ec != std::errc{} || end != value.data() + value.size() || shards == 0) {
                return std::unexpected(std::string{"--shards must be a number >= 1"});
            }
            options.shards = shards;
            shards_given = true;
        } else if (key == "--config") {
            if (value.empty()) {
                return std::unexpected(std::string{"--config: a file path is required"});
            }
            options.config_file = value;
            config_given = true;
        } else {
            return std::unexpected("unknown option '" + std::string{arg} + "'");
        }
    }
    if (config_given && (instruments_given || shards_given)) {
        return std::unexpected(
            std::string{"--config and --instruments/--shards are mutually exclusive"});
    }
    if (!options.help && !journal_dir_given) {
        return std::unexpected(std::string{"--journal-dir is required"});
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) try {
    const auto options = parse_options(std::span{argv, static_cast<std::size_t>(argc)});
    if (!options) {
        std::println(stderr, "error: {}\n{}", options.error(), usage);
        return exit_usage;
    }
    if (options->help) {
        std::println("{}", usage);
        return EXIT_SUCCESS;
    }

    const auto engine_config =
        main_app::build_engine_config(options->config_file, options->instruments, options->shards);
    if (!engine_config) {
        std::println(stderr, "lockstep-replay: config: {}", engine_config.error());
        return exit_usage;
    }

    const auto router =
        app::Router::round_robin(engine_config->instruments, engine_config->shard_count);
    if (const auto valid = main_app::validate_journal_dir(
            options->journal_dir, static_cast<std::uint32_t>(engine_config->shard_count));
        !valid) {
        std::println(stderr, "lockstep-replay: {}", valid.error());
        return exit_journal_error;
    }
    bool failed = false;
    for (std::uint32_t s = 0; s < engine_config->shard_count; ++s) {
        const domain::ShardId shard{s};
        const auto result =
            main_app::compute_shard_digest(options->journal_dir, router, shard,
                                           static_cast<std::uint32_t>(engine_config->shard_count),
                                           engine_config->risk_link_policy);
        if (!result) {
            std::println(stderr, "lockstep-replay: {}", result.error());
            failed = true;
            continue;
        }
        std::println("{}", main_app::format_shard_digest_line(result->line));
        if (result->torn_tail) {
            std::println(stderr, "lockstep-replay: shard {}: torn tail, journal ends there", s);
        }
    }
    return failed ? exit_journal_error : EXIT_SUCCESS;
} catch (const std::exception& e) {
    (void)std::fputs("lockstep-replay: ", stderr);
    (void)std::fputs(e.what(), stderr);
    (void)std::fputs("\n", stderr);
    return exit_journal_error;
} catch (...) {
    return exit_journal_error;
}
