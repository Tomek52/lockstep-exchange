#include "lockstep/journal/restart.hpp"

#include <filesystem>
#include <string>
#include <utility>

#include "lockstep/app/ports/journal.hpp"
#include "lockstep/journal/file_journal_reader.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/record_codec.hpp"

namespace lockstep::journal {

std::expected<ShardRestart, std::string> recover_for_restart(const std::filesystem::path& dir,
                                                             const domain::ShardConfig& config,
                                                             std::uint32_t shard_count) {
    const std::filesystem::path path = dir / FileJournalWriter::file_name(config.shard);
    if (!std::filesystem::exists(path)) {
        return ShardRestart{};  // fresh start: no journal to resume
    }

    const auto recovered = recover_tail(path);
    if (!recovered) {
        return std::unexpected("journal: cannot restart on '" + path.string() +
                               "': " + std::string(app::to_string(recovered.error())));
    }

    const ReaderExpectations expect{
        .shard = config.shard, .shard_count = shard_count, .config_hash = config_hash(config)};
    ShardRestart result;
    result.existed = true;
    // Sequence numbers start at 1 and increase by exactly 1 (ADR-0004: they
    // come from the shard's own counter, which only ever increments by one
    // per command). A gap or a restart of the counter means the file was
    // produced by something other than a single ShardRuntime run - resuming
    // it would continue sequence numbering from the wrong place, so this is
    // refused before a single command is replayed, naming the file and the
    // offending sequence. read_journal's own contract is unchanged: this
    // check belongs to the caller, not the reader.
    std::uint64_t expected_sequence = 1;
    for (auto&& record : read_journal(path, expect)) {
        if (!record) {
            return std::unexpected("journal: cannot restart on '" + path.string() +
                                   "': " + std::string(app::to_string(record.error())));
        }
        if (record->sequence.value() != expected_sequence) {
            return std::unexpected("journal: cannot restart on '" + path.string() + "': sequence " +
                                   std::to_string(record->sequence.value()) +
                                   " is not the expected " + std::to_string(expected_sequence));
        }
        result.resume_commands.push_back(*record);
        ++expected_sequence;
    }
    return result;
}

}  // namespace lockstep::journal
