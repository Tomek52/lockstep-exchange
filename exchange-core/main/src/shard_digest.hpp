#pragma once

// Computes and prints one shard's digest line from its journal file on disk.
// Shared by exchange-core's --print-digest-on-exit and the lockstep-replay
// tool, so the two are guaranteed to agree bit for bit on the same journal
// directory (ADR-0020's "digest on exit" rationale): both replay the file
// through the exact same code path.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "lockstep/app/router.hpp"
#include "lockstep/domain/risk_state.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::main_app {

/// One line of `lockstep-replay`/`--print-digest-on-exit` output.
struct ShardDigestLine {
    domain::ShardId shard;
    std::uint64_t commands{0};
    std::uint64_t events{0};
    std::uint64_t digest{0};
};

struct ShardDigestResult {
    ShardDigestLine line;
    /// True if the file ended in a torn tail (a crash mid-write): reported,
    /// then treated as end of input - the digest covers every complete
    /// record before it. False for a cleanly closed journal.
    bool torn_tail{false};
};

/// Reads shard `shard`'s journal file from `journal_dir`, replays it into a
/// fresh domain::ShardEngine built from `router`/`shard_count`/`policy`, and
/// returns its digest (app::digest) over every event, reply and final book
/// snapshot. A torn tail at the very end is reported in the result and
/// otherwise treated as end of input; any other journal error (missing file,
/// corrupt, version or config mismatch) is returned as a message naming the
/// file.
[[nodiscard]] std::expected<ShardDigestResult, std::string> compute_shard_digest(
    const std::filesystem::path& journal_dir,
    const app::Router& router,
    domain::ShardId shard,
    std::uint32_t shard_count,
    domain::RiskLinkPolicy policy);

/// Formats one line exactly as the task's examples show it:
/// "shard 0: 12345 commands, 20211 events, digest=0x3f2a...".
[[nodiscard]] std::string format_shard_digest_line(const ShardDigestLine& line);

/// Validates `journal_dir` against `shard_count` before exchange-core
/// restarts on it or lockstep-replay reads it (task 010 review F6):
///
///  - a `shard-<id>.jnl` file for an id >= shard_count is refused, naming
///    the file: the header's own shard_count would disagree with such a
///    file anyway (ADR-0012/ADR-0017), and silently ignoring it (it is
///    outside 0..shard_count-1, so nothing reads it) would hide that the
///    directory does not match this run's configuration;
///  - if any shard in 0..shard_count-1 has a journal file, every shard in
///    that range must, naming whichever are missing: a partial set is not
///    a fresh start for the missing ones, it is a directory that does not
///    match a single, consistent exchange run.
[[nodiscard]] std::expected<void, std::string> validate_journal_dir(
    const std::filesystem::path& journal_dir, std::uint32_t shard_count);

}  // namespace lockstep::main_app
