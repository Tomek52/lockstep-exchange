// Acceptance criterion 4: truncate a shard journal mid-record (simulating a
// crash), recover_tail() it, then replay - the result equals the live
// output up to the last complete record, and nothing after the cut point is
// expected to appear.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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

class CrashRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-crash-" + std::to_string(::getpid()) + "-" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }
    [[nodiscard]] const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
};

TEST_F(CrashRecoveryTest, RecoveredTailReplaysUpToTheLastCompleteRecord) {
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
    test::run_workload(engine, subscriber, live_replies, /*producer_count=*/2,
                       /*commands_per_producer=*/400);

    const ShardId shard{1};  // instrument 2 (the busiest in e2e-smoke) lives here
    const auto path = dir() / journal::FileJournalWriter::file_name(shard);
    const auto full_size = fs::file_size(path);
    ASSERT_GT(full_size, journal::file_header_size + journal::record_header_size)
        << "need at least one record to truncate meaningfully";

    // Simulate a crash partway through writing a record: cut somewhere in
    // the back third of the file, which (for records of a few dozen bytes
    // each) is virtually guaranteed to land inside a record's header or
    // payload rather than exactly on a boundary.
    const auto cut_at = journal::file_header_size + (full_size - journal::file_header_size) * 2 / 3;
    fs::resize_file(path, cut_at);

    const auto recovered = journal::recover_tail(path);
    ASSERT_TRUE(recovered.has_value()) << app::to_string(recovered.error());
    ASSERT_GT(*recovered, 0U) << "the truncation point must leave at least one complete record";
    ASSERT_LT(fs::file_size(path), full_size) << "recover_tail must have cut the torn tail";

    const auto instruments = router.instruments_of(shard);
    const ShardConfig shard_config{.shard = shard,
                                   .instruments = {instruments.begin(), instruments.end()}};
    const journal::ReaderExpectations expect{.shard = shard,
                                             .shard_count = shard_count,
                                             .config_hash = journal::config_hash(shard_config)};

    std::vector<SequencedCommand> recovered_commands;
    for (auto&& record : journal::read_journal(path, expect)) {
        ASSERT_TRUE(record.has_value()) << app::to_string(record.error());
        recovered_commands.push_back(*record);
    }
    ASSERT_EQ(recovered_commands.size(), *recovered);
    const std::uint64_t last_recovered_sequence = recovered_commands.back().sequence.value();

    ShardEngine fresh{shard_config};
    const app::ReplayOutput replayed = app::replay(fresh, recovered_commands);

    // The live run's own output, restricted to sequence numbers that
    // survived the truncation: this is the "live output up to the last
    // complete record" the acceptance criterion asks for.
    const auto live_events_prefix =
        subscriber.events() | std::views::filter([&](const auto& e) {
            return e.shard == shard && e.sequence.value() <= last_recovered_sequence;
        }) |
        std::ranges::to<std::vector>();
    auto live_replies_prefix =
        live_replies | std::views::filter([&](const auto& r) {
            return r.shard == shard && r.sequence.value() <= last_recovered_sequence;
        }) |
        std::ranges::to<std::vector>();

    ASSERT_EQ(replayed.events.size(), live_events_prefix.size());
    for (const auto& [live, again] : std::views::zip(live_events_prefix, replayed.events)) {
        ASSERT_EQ(live, again) << "first divergence at shard sequence " << live.sequence.value();
    }
    std::map<std::uint64_t, app::CommandReply> replayed_by_seq;
    for (const auto& reply : replayed.replies) {
        replayed_by_seq.emplace(reply.sequence.value(), reply);
    }
    for (const auto& reply : live_replies_prefix) {
        ASSERT_EQ(replayed_by_seq.at(reply.sequence.value()), reply);
    }
    // Nothing past the cut leaked in.
    for (const auto& reply : replayed.replies) {
        EXPECT_LE(reply.sequence.value(), last_recovered_sequence);
    }
}

}  // namespace
}  // namespace lockstep
