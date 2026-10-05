// Acceptance criterion 5 (ADR-0020): run, stop, restart on the same journal
// directory, submit more commands, stop again - then a fresh replay of the
// whole directory reproduces the restarted engine's final book state and
// gives a stable digest, exactly as a third restart or `lockstep-replay`
// would see it.
#include <array>
#include <cstddef>
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
#include "lockstep/journal/restart.hpp"

#include "app/test_support.hpp"
#include "generator.hpp"
#include <unistd.h>

namespace lockstep {
namespace {

using namespace domain;
namespace fs = std::filesystem;

constexpr std::size_t shard_count = 2;

class RestartDeterminismTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-restart-det-" + std::to_string(::getpid()) + "-" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }
    [[nodiscard]] const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
};

/// Replays the whole of `dir`'s shard `shard` into a fresh ShardEngine, the
/// same way lockstep-replay / --print-digest-on-exit do: every record from
/// the journal on disk, nothing held back.
app::ReplayOutput replay_whole_file(const fs::path& dir,
                                    const app::Router& router,
                                    ShardEngine& fresh,
                                    ShardId shard) {
    const auto instruments = router.instruments_of(shard);
    const ShardConfig shard_config{.shard = shard,
                                   .instruments = {instruments.begin(), instruments.end()}};
    const journal::ReaderExpectations expect{.shard = shard,
                                             .shard_count = shard_count,
                                             .config_hash = journal::config_hash(shard_config)};
    std::vector<SequencedCommand> commands;
    std::uint64_t expected_sequence = 1;
    for (auto&& record :
         journal::read_journal(dir / journal::FileJournalWriter::file_name(shard), expect)) {
        EXPECT_TRUE(record.has_value()) << app::to_string(record.error());
        if (record.has_value()) {
            // Task 010 review F3: the whole directory, across both runs,
            // must still number sequences 1, 2, 3, ... with no gap or
            // restart - exactly what recover_for_restart/compute_shard_digest
            // check on a real restart; checked here too so a dropped
            // sequence_ update in resume_from_journal fails this test, not
            // only a lower-level one.
            EXPECT_EQ(record->sequence.value(), expected_sequence)
                << "shard " << shard.value() << ": sequence gap or restart in the combined file";
            ++expected_sequence;
            commands.push_back(*record);
        }
    }
    return app::replay(fresh, commands);
}

TEST_F(RestartDeterminismTest, RestartedEngineMatchesAFreshReplayOfTheWholeDirectory) {
    const app::Router router = app::Router::round_robin(test::instruments(), shard_count);

    // Run 1: a fresh exchange.
    {
        app::ManualClock clock;
        test::RecordingSubscriber subscriber;
        std::vector<app::CommandReply> replies;
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
        test::run_workload(engine, subscriber, replies, /*producer_count=*/2,
                           /*commands_per_producer=*/200);
        // engine (and its journals) close here, as a real restart requires.
    }

    // Run 2: restart on the same directory (ADR-0020's own recover/replay/
    // resume building blocks, exactly as main.cpp uses them), then submit
    // more commands.
    std::vector<domain::BookSnapshot> live_books_by_shard_then_instrument;
    test::RecordingSubscriber run2_subscriber;
    std::vector<app::CommandReply> run2_replies;
    std::array<std::uint64_t, shard_count> last_sequence_from_run1{};
    {
        app::ManualClock clock;

        std::vector<journal::ShardRestart> shard_restarts;
        for (std::uint32_t s = 0; s < shard_count; ++s) {
            const ShardId shard{s};
            const auto instruments = router.instruments_of(shard);
            const ShardConfig shard_config{.shard = shard,
                                           .instruments = {instruments.begin(), instruments.end()}};
            auto recovered = journal::recover_for_restart(dir(), shard_config, shard_count);
            ASSERT_TRUE(recovered.has_value()) << recovered.error();
            ASSERT_TRUE(recovered->existed) << "shard " << s << " should have a journal from run 1";
            ASSERT_FALSE(recovered->resume_commands.empty());
            // N1 (acceptance criterion 5 / review F3): the last sequence
            // run 1 produced on this shard, so run 2's own live output can
            // be told apart from what it merely resumed.
            last_sequence_from_run1[s] = recovered->resume_commands.back().sequence.value();
            shard_restarts.push_back(std::move(*recovered));
        }

        auto journal_factory = [this](ShardId shard) -> std::unique_ptr<app::Journal> {
            return journal::FileJournalWriter::open_for_append(dir(), shard,
                                                               journal::SyncPolicy::None);
        };
        app::ResumeFactory resume_factory = [&shard_restarts](ShardId shard) {
            return std::move(shard_restarts.at(shard.value()).resume_commands);
        };
        app::Engine engine{
            app::EngineConfig{.instruments = test::instruments(), .shard_count = shard_count},
            std::move(journal_factory), clock, std::move(resume_factory)};
        test::run_workload(engine, run2_subscriber, run2_replies, /*producer_count=*/2,
                           /*commands_per_producer=*/200);

        for (std::uint32_t s = 0; s < shard_count; ++s) {
            const ShardId shard{s};
            for (const InstrumentSpec& spec : router.instruments_of(shard)) {
                const OrderBook* book = engine.shard(shard).engine().book(spec.id);
                ASSERT_NE(book, nullptr);
                live_books_by_shard_then_instrument.push_back(book->snapshot());
            }
        }
    }

    // A fresh replay of the whole directory (both runs) must reproduce the
    // restarted engine's final book state exactly, and do so stably.
    std::vector<domain::BookSnapshot> replayed_books;
    std::vector<app::ReplayOutput> replayed_outputs_by_shard;
    std::uint64_t digest_first = 0;
    std::uint64_t digest_second = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<domain::BookSnapshot> books_this_attempt;
        std::vector<app::ReplayOutput> outputs_this_attempt;
        std::uint64_t combined_digest = 0;  // combined across shards, order-sensitive per shard
        for (std::uint32_t s = 0; s < shard_count; ++s) {
            const ShardId shard{s};
            ShardEngine fresh{ShardConfig{.shard = shard,
                                          .instruments = {router.instruments_of(shard).begin(),
                                                          router.instruments_of(shard).end()}}};
            app::ReplayOutput output = replay_whole_file(dir(), router, fresh, shard);
            std::vector<domain::BookSnapshot> shard_books;
            for (const InstrumentSpec& spec : router.instruments_of(shard)) {
                const OrderBook* book = fresh.book(spec.id);
                ASSERT_NE(book, nullptr);
                shard_books.push_back(book->snapshot());
                books_this_attempt.push_back(book->snapshot());
            }
            // Folds every shard's digest together with FNV-1a-style mixing
            // via app::digest itself (order-sensitive: shard 0 then shard 1,
            // matching lockstep-replay's iteration order).
            combined_digest =
                app::digest(output, shard_books) ^ (combined_digest * 1'099'511'628'211ULL);
            outputs_this_attempt.push_back(std::move(output));
        }
        if (attempt == 0) {
            replayed_books = books_this_attempt;
            replayed_outputs_by_shard = std::move(outputs_this_attempt);
            digest_first = combined_digest;
        } else {
            digest_second = combined_digest;
            EXPECT_EQ(books_this_attempt, replayed_books)
                << "replaying the restarted directory twice must agree";
        }
    }
    EXPECT_EQ(digest_first, digest_second);

    ASSERT_EQ(live_books_by_shard_then_instrument.size(), replayed_books.size());
    for (std::size_t i = 0; i < replayed_books.size(); ++i) {
        EXPECT_EQ(live_books_by_shard_then_instrument[i], replayed_books[i]) << "book index " << i;
    }

    // Acceptance criterion 5 / review F3: run 2's own live events and
    // replies (sequence > N1) must match the full-directory replay
    // restricted to the same range - the same style of comparison
    // crash_recovery_test.cpp uses for a torn tail's prefix, here for a
    // restart's suffix.
    for (std::uint32_t s = 0; s < shard_count; ++s) {
        const ShardId shard{s};
        const std::uint64_t n1 = last_sequence_from_run1[s];
        const auto live_events_suffix = run2_subscriber.events() |
                                        std::views::filter([&](const auto& e) {
                                            return e.shard == shard && e.sequence.value() > n1;
                                        }) |
                                        std::ranges::to<std::vector>();
        auto live_replies_suffix = run2_replies | std::views::filter([&](const auto& r) {
                                       return r.shard == shard && r.sequence.value() > n1;
                                   }) |
                                   std::ranges::to<std::vector>();
        const auto replayed_events_suffix =
            replayed_outputs_by_shard[s].events |
            std::views::filter([&](const auto& e) { return e.sequence.value() > n1; }) |
            std::ranges::to<std::vector>();

        ASSERT_EQ(live_events_suffix.size(), replayed_events_suffix.size()) << "shard " << s;
        for (const auto& [live, again] :
             std::views::zip(live_events_suffix, replayed_events_suffix)) {
            ASSERT_EQ(live, again)
                << "shard " << s << ": first divergence after restart at sequence "
                << live.sequence.value();
        }
        std::map<std::uint64_t, app::CommandReply> replayed_by_seq;
        for (const auto& reply : replayed_outputs_by_shard[s].replies) {
            if (reply.sequence.value() > n1) {
                replayed_by_seq.emplace(reply.sequence.value(), reply);
            }
        }
        for (const auto& reply : live_replies_suffix) {
            ASSERT_EQ(replayed_by_seq.at(reply.sequence.value()), reply) << "shard " << s;
        }
    }
}

}  // namespace
}  // namespace lockstep
