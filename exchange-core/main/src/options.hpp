#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace lockstep::main_app {

struct Options {
    std::string listen{"0.0.0.0:50051"};
    std::string risk_target{"localhost:50052"};
    std::string exchange_id{"exchange-core-1"};
    std::size_t shards{2};
    std::vector<std::uint32_t> instruments{1, 2, 3, 4};
    bool risk_enabled{true};
    std::filesystem::path journal_dir{"journal"};
    bool fsync_every_commit{true};
    bool help{false};
};

inline constexpr const char* usage = R"(usage: exchange-core [options]
  --listen=HOST:PORT          order entry / market data address   (default 0.0.0.0:50051)
  --risk-sentinel=HOST:PORT   risk-sentinel address                (default localhost:50052)
  --no-risk                   do not connect to risk-sentinel
  --exchange-id=ID            identity sent in the risk handshake  (default exchange-core-1)
  --shards=N                  number of shard threads              (default 2)
  --instruments=ID,ID,...     instrument ids to list               (default 1,2,3,4)
  --journal-dir=DIR           directory for shard-<id>.jnl files   (default ./journal)
  --fsync=none|commit         fdatasync journal on every commit    (default commit)
  --help)";

/// Parses `--key=value` style arguments. Startup-time code: errors are
/// reported as a message for the user, not as exceptions.
[[nodiscard]] std::expected<Options, std::string> parse_options(std::span<char* const> args);

}  // namespace lockstep::main_app
