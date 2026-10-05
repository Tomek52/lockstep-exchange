#include "shard_digest.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <string_view>
#include <vector>

#include "lockstep/app/digest.hpp"
#include "lockstep/app/ports/journal.hpp"
#include "lockstep/app/replay.hpp"
#include "lockstep/domain/order_book.hpp"
#include "lockstep/domain/shard_engine.hpp"
#include "lockstep/journal/file_journal_reader.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/record_codec.hpp"

namespace lockstep::main_app {

std::expected<ShardDigestResult, std::string> compute_shard_digest(
    const std::filesystem::path& journal_dir,
    const app::Router& router,
    domain::ShardId shard,
    std::uint32_t shard_count,
    domain::RiskLinkPolicy policy) {
    const auto instruments = router.instruments_of(shard);
    const domain::ShardConfig config{.shard = shard,
                                     .instruments = {instruments.begin(), instruments.end()},
                                     .risk_link_policy = policy};
    const auto path = journal_dir / journal::FileJournalWriter::file_name(shard);
    const journal::ReaderExpectations expect{
        .shard = shard, .shard_count = shard_count, .config_hash = journal::config_hash(config)};

    domain::ShardEngine engine{config};
    app::ReplayOutput output;
    domain::EventBuffer buffer;
    std::uint64_t commands = 0;
    bool torn_tail = false;
    // Sequence numbers start at 1 and increase by exactly 1 (see
    // recover_for_restart's identical check and comment); read_journal's
    // own contract is unchanged, this belongs to the caller.
    std::uint64_t expected_sequence = 1;

    for (auto&& record : journal::read_journal(path, expect)) {
        if (!record) {
            // A torn tail is only a benign "stop here" once at least one
            // complete record was read; a file shorter than the header, or
            // one truncated before its very first record, has nothing to
            // report and is an error, consistent with exchange-core
            // startup's own recover_for_restart refusing the same case.
            if (record.error() == app::JournalError::Truncated && commands > 0) {
                torn_tail = true;
                break;
            }
            return std::unexpected(path.string() + ": " +
                                   std::string(app::to_string(record.error())));
        }
        if (record->sequence.value() != expected_sequence) {
            return std::unexpected(path.string() + ": sequence " +
                                   std::to_string(record->sequence.value()) +
                                   " is not the expected " + std::to_string(expected_sequence));
        }
        buffer.clear();
        const domain::CommandResult result = engine.apply(*record, buffer);
        for (const domain::Event& event : buffer.events()) {
            output.events.push_back(
                app::PublishedEvent{shard, record->sequence, record->timestamp, event});
        }
        output.replies.push_back(
            app::CommandReply{shard, record->sequence, record->timestamp, result});
        ++commands;
        ++expected_sequence;
    }

    std::vector<domain::BookSnapshot> books;
    books.reserve(instruments.size());
    for (const domain::InstrumentSpec& spec : instruments) {
        if (const domain::OrderBook* book = engine.book(spec.id)) {
            books.push_back(book->snapshot());
        }
    }

    return ShardDigestResult{.line = {.shard = shard,
                                      .commands = commands,
                                      .events = output.events.size(),
                                      .digest = app::digest(output, books)},
                             .torn_tail = torn_tail};
}

std::string format_shard_digest_line(const ShardDigestLine& line) {
    return std::format("shard {}: {} commands, {} events, digest={:#018x}", line.shard.value(),
                       line.commands, line.events, line.digest);
}

std::expected<void, std::string> validate_journal_dir(const std::filesystem::path& journal_dir,
                                                      std::uint32_t shard_count) {
    std::error_code ec;
    if (!std::filesystem::is_directory(journal_dir, ec)) {
        return {};  // nothing to validate yet; the usual create() path handles this
    }

    std::vector<bool> present(shard_count, false);
    for (const auto& entry : std::filesystem::directory_iterator{journal_dir, ec}) {
        if (ec) {
            break;
        }
        const std::string filename = entry.path().filename().string();
        constexpr std::string_view prefix = "shard-";
        constexpr std::string_view suffix = ".jnl";
        if (!filename.starts_with(prefix) || !filename.ends_with(suffix)) {
            continue;  // not a journal file; not this function's concern
        }
        const std::string_view digits{filename.data() + prefix.size(),
                                      filename.size() - prefix.size() - suffix.size()};
        std::uint32_t id{};
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
            continue;  // doesn't parse as "shard-<digits>.jnl"; not ours to judge
        }
        if (id >= shard_count) {
            return std::unexpected(
                "journal: '" + entry.path().string() + "' is for shard " + std::to_string(id) +
                ", outside this run's shard_count=" + std::to_string(shard_count));
        }
        present[id] = true;
    }

    const bool any_present = std::ranges::any_of(present, std::identity{});
    const bool all_present = std::ranges::all_of(present, std::identity{});
    if (any_present && !all_present) {
        std::string missing;
        for (std::uint32_t id = 0; id < shard_count; ++id) {
            if (!present[id]) {
                if (!missing.empty()) {
                    missing += ", ";
                }
                missing += journal::FileJournalWriter::file_name(domain::ShardId{id}).string();
            }
        }
        return std::unexpected("journal: '" + journal_dir.string() +
                               "' has some but not all shard journals; missing: " + missing);
    }
    return {};
}

}  // namespace lockstep::main_app
