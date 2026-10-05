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
    for (auto&& record : read_journal(path, expect)) {
        if (!record) {
            return std::unexpected("journal: cannot restart on '" + path.string() +
                                   "': " + std::string(app::to_string(record.error())));
        }
        result.resume_commands.push_back(*record);
    }
    return result;
}

}  // namespace lockstep::journal
