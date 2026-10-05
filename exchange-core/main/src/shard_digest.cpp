#include "shard_digest.hpp"

#include <format>
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

    for (auto&& record : journal::read_journal(path, expect)) {
        if (!record) {
            if (record.error() == app::JournalError::Truncated) {
                torn_tail = true;
                break;
            }
            return std::unexpected(path.string() + ": " +
                                   std::string(app::to_string(record.error())));
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

}  // namespace lockstep::main_app
