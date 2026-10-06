#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/shard_engine.hpp"

namespace lockstep::journal {

/// What was found for one shard's journal file at startup (ADR-0020).
struct ShardRestart {
    /// Empty when no journal file existed for this shard (a fresh start).
    /// Otherwise every record recovered from the existing file, in order:
    /// replay them into the shard's engine before its thread starts, and
    /// resume sequence numbering after the last one.
    std::vector<domain::SequencedCommand> resume_commands;
    bool existed{false};
};

/// Recovers and validates shard `config.shard`'s journal file in `dir`, if
/// one exists, for resuming it rather than failing on O_EXCL (ADR-0020):
///
///  1. recover_tail() cuts a torn tail; it never touches a file it refuses.
///  2. The recovered file is read back expecting `config.shard`,
///     `shard_count` and `config.`'s config_hash (ADR-0017).
///
/// On any refusal - recover_tail's Corrupt/VersionMismatch/IoFailure, or a
/// header mismatch found while reading back (ConfigMismatch) - returns a
/// message naming the file. The caller must stop startup; this function
/// never creates, deletes or truncates the file itself beyond what
/// recover_tail does to a torn tail. Must not run while a writer or another
/// recoverer has the file open (recover_tail's precondition).
[[nodiscard]] std::expected<ShardRestart, std::string> recover_for_restart(
    const std::filesystem::path& dir, const domain::ShardConfig& config, std::uint32_t shard_count);

}  // namespace lockstep::journal
