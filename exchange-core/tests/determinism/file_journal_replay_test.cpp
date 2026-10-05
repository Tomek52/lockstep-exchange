// Acceptance criterion 2: a live run through FileJournalWriter in a temp
// directory, replayed via journal::read_journal -> app::replay, reproduces
// every shard's events, replies and final book snapshot exactly - the same
// property replay_determinism_test.cpp proves for MemoryJournal, now over
// the real on-disk format (ADR-0012).
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/app/digest.hpp"
#include "lockstep/app/replay.hpp"
#include "lockstep/journal/file_journal_reader.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/record_codec.hpp"

#include "app/test_support.hpp"
#include "generator.hpp"
#include <unistd.h>

namespace lockstep {
namespace {

using namespace domain;
namespace fs = std::filesystem;

constexpr std::size_t shard_count = 2;

class FileJournalReplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-det-" + std::to_string(::getpid()) + "-" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }
    [[nodiscard]] const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
};

TEST_F(FileJournalReplayTest, LiveRunReplaysIdenticallyFromDisk) {
    app::ManualClock clock;
    test::RecordingSubscriber subscriber;
    std::vector<app::CommandReply> live_replies;

    const app::Router router = app::Router::round_robin(test::instruments(), shard_count);
    auto journal_factory = [this, &router](ShardId shard) -> std::unique_ptr<app::Journal> {
        const auto instruments = router.instruments_of(shard);
        const ShardConfig shard_config{.shard = shard,
                                       .instruments = {instruments.begin(), instruments.end()}};
        return journal::FileJournalWriter::create(
            dir(),
            journal::FileHeader{.shard = shard,
                                .shard_count = shard_count,
                                .config_hash = journal::config_hash(shard_config)},
            journal::SyncPolicy::None);
    };
    app::Engine engine{
        app::EngineConfig{.instruments = test::instruments(), .shard_count = shard_count},
        std::move(journal_factory), clock};
    // Fewer commands than the in-memory test: file I/O makes this slower,
    // and the property under test is the codec/reader round trip, not scale.
    test::run_workload(engine, subscriber, live_replies, /*producer_count=*/3,
                       /*commands_per_producer=*/500);

    for (std::uint32_t s = 0; s < shard_count; ++s) {
        const ShardId shard{s};
        const auto owned = router.instruments_of(shard);
        const ShardConfig shard_config{.shard = shard, .instruments = {owned.begin(), owned.end()}};
        const journal::ReaderExpectations expect{.shard = shard,
                                                 .shard_count = shard_count,
                                                 .config_hash = journal::config_hash(shard_config)};

        std::vector<SequencedCommand> commands;
        for (auto&& record :
             journal::read_journal(dir() / journal::FileJournalWriter::file_name(shard), expect)) {
            ASSERT_TRUE(record.has_value()) << app::to_string(record.error());
            commands.push_back(*record);
        }
        ASSERT_FALSE(commands.empty());

        ShardEngine fresh{shard_config};
        const app::ReplayOutput replayed = app::replay(fresh, commands);

        const auto live_events =
            subscriber.events() |
            std::views::filter([&](const auto& e) { return e.shard == shard; }) |
            std::ranges::to<std::vector>();
        auto live_shard_replies =
            live_replies | std::views::filter([&](const auto& r) { return r.shard == shard; }) |
            std::ranges::to<std::vector>();

        ASSERT_EQ(replayed.events.size(), live_events.size());
        for (const auto& [live, again] : std::views::zip(live_events, replayed.events)) {
            ASSERT_EQ(live, again)
                << "first divergence at shard sequence " << live.sequence.value();
        }
        std::map<std::uint64_t, app::CommandReply> replayed_by_seq;
        for (const auto& reply : replayed.replies) {
            replayed_by_seq.emplace(reply.sequence.value(), reply);
        }
        for (const auto& reply : live_shard_replies) {
            ASSERT_EQ(replayed_by_seq.at(reply.sequence.value()), reply);
        }

        for (const InstrumentSpec& spec : owned) {
            const OrderBook* live_book = engine.shard(shard).engine().book(spec.id);
            const OrderBook* replayed_book = fresh.book(spec.id);
            ASSERT_NE(live_book, nullptr);
            ASSERT_NE(replayed_book, nullptr);
            EXPECT_EQ(live_book->snapshot(), replayed_book->snapshot())
                << "instrument " << spec.id.value();
        }
    }
}

// Acceptance criterion 10 (task 010 review F1): a live run's own digest
// (ShardRuntime's DigestBuilder, folded incrementally as the run produced
// its output) must equal a from-disk replay's digest of the resulting
// journal - not by construction (the two are computed by entirely
// different code paths: one incremental and live, one batch and from
// app::replay()), which is exactly what would catch a bug in how, or
// whether, the live path folds its output.
TEST_F(FileJournalReplayTest, LiveDigestEqualsDiskReplayDigest) {
    app::ManualClock clock;
    test::RecordingSubscriber subscriber;
    std::vector<app::CommandReply> live_replies;

    const app::Router router = app::Router::round_robin(test::instruments(), shard_count);
    auto journal_factory = [this, &router](ShardId shard) -> std::unique_ptr<app::Journal> {
        const auto instruments = router.instruments_of(shard);
        const ShardConfig shard_config{.shard = shard,
                                       .instruments = {instruments.begin(), instruments.end()}};
        return journal::FileJournalWriter::create(
            dir(),
            journal::FileHeader{.shard = shard,
                                .shard_count = shard_count,
                                .config_hash = journal::config_hash(shard_config)},
            journal::SyncPolicy::None);
    };
    app::Engine engine{
        app::EngineConfig{
            .instruments = test::instruments(), .shard_count = shard_count, .record_digest = true},
        std::move(journal_factory), clock};
    test::run_workload(engine, subscriber, live_replies, /*producer_count=*/3,
                       /*commands_per_producer=*/300);

    for (std::uint32_t s = 0; s < shard_count; ++s) {
        const ShardId shard{s};
        const auto owned = router.instruments_of(shard);
        const ShardConfig shard_config{.shard = shard, .instruments = {owned.begin(), owned.end()}};

        // Live: the digest this shard's own ShardRuntime accumulated while
        // it ran, finished with book snapshots taken after Engine::stop()
        // joined every shard thread (run_workload() already called stop()).
        std::vector<BookSnapshot> live_books;
        for (const InstrumentSpec& spec : owned) {
            const OrderBook* book = engine.shard(shard).engine().book(spec.id);
            ASSERT_NE(book, nullptr);
            live_books.push_back(book->snapshot());
        }
        ASSERT_TRUE(engine.shard(shard).digest_builder().has_value());
        const std::uint64_t live_digest = engine.shard(shard).digest_builder()->finish(live_books);

        // From disk: read the journal back and replay it into a fresh
        // engine, the way lockstep-replay does.
        const journal::ReaderExpectations expect{.shard = shard,
                                                 .shard_count = shard_count,
                                                 .config_hash = journal::config_hash(shard_config)};
        std::vector<SequencedCommand> commands;
        for (auto&& record :
             journal::read_journal(dir() / journal::FileJournalWriter::file_name(shard), expect)) {
            ASSERT_TRUE(record.has_value()) << app::to_string(record.error());
            commands.push_back(*record);
        }
        ShardEngine fresh{shard_config};
        const app::ReplayOutput replayed = app::replay(fresh, commands);
        std::vector<BookSnapshot> replayed_books;
        for (const InstrumentSpec& spec : owned) {
            const OrderBook* book = fresh.book(spec.id);
            ASSERT_NE(book, nullptr);
            replayed_books.push_back(book->snapshot());
        }
        const std::uint64_t replayed_digest = app::digest(replayed, replayed_books);

        EXPECT_EQ(live_digest, replayed_digest) << "shard " << s;
    }
}

}  // namespace
}  // namespace lockstep
