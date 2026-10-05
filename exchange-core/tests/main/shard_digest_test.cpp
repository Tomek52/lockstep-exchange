// compute_shard_digest (shard_digest.hpp), shared by lockstep-replay and
// --print-digest-on-exit: a torn tail is reported and treated as end of
// input (acceptance criterion 6), any other journal error is an error.
#include "shard_digest.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/shard_engine.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/record_codec.hpp"

#include <unistd.h>

namespace lockstep::main_app {
namespace {

using namespace domain;
namespace fs = std::filesystem;

constexpr std::uint32_t shard_count = 1;

class ShardDigestTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-shard-digest-" + std::to_string(::getpid()) + "-" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);

        router_ = app::Router::round_robin(instruments(), shard_count);
        const auto instruments_of_shard = router_.instruments_of(ShardId{0});
        const ShardConfig config{
            .shard = ShardId{0},
            .instruments = {instruments_of_shard.begin(), instruments_of_shard.end()}};
        const auto writer = journal::FileJournalWriter::create(
            dir_,
            journal::FileHeader{.shard = ShardId{0},
                                .shard_count = shard_count,
                                .config_hash = journal::config_hash(config)},
            journal::SyncPolicy::EveryCommit);
        for (std::uint64_t i = 0; i < 3; ++i) {
            const SequencedCommand command{SequenceNumber{i + 1},
                                           Timestamp{static_cast<std::int64_t>(1'000 + i)},
                                           NewOrder{.trader = TraderId{1},
                                                    .client_order_id = ClientOrderId{i + 1},
                                                    .instrument = InstrumentId{1},
                                                    .price = Price{10},
                                                    .quantity = Quantity{1}}};
            ASSERT_TRUE(writer->append(command).has_value());
        }
        ASSERT_TRUE(writer->commit().has_value());
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] static std::vector<InstrumentSpec> instruments() {
        return {InstrumentSpec{.id = InstrumentId{1}}};
    }

    [[nodiscard]] const fs::path& dir() const { return dir_; }
    [[nodiscard]] fs::path path() const {
        return dir_ / journal::FileJournalWriter::file_name(ShardId{0});
    }

    app::Router router_;

private:
    fs::path dir_;
};

TEST_F(ShardDigestTest, CleanJournalReportsNoTornTail) {
    const auto result =
        compute_shard_digest(dir(), router_, ShardId{0}, shard_count, RiskLinkPolicy::FailOpen);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_FALSE(result->torn_tail);
    EXPECT_EQ(result->line.commands, 3U);
}

// Acceptance criterion 6: a torn tail is reported, not an error, and treated
// as end of input - the digest covers every complete record before it.
TEST_F(ShardDigestTest, TornTailIsReportedAndTreatedAsEndOfInput) {
    const auto full_size = fs::file_size(path());
    fs::resize_file(path(), full_size - 2);  // cut 2 bytes off the last record

    const auto result =
        compute_shard_digest(dir(), router_, ShardId{0}, shard_count, RiskLinkPolicy::FailOpen);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->torn_tail);
    EXPECT_EQ(result->line.commands, 2U) << "only the 2 complete records before the torn one";
}

TEST_F(ShardDigestTest, MissingJournalIsAnError) {
    fs::remove(path());
    const auto result =
        compute_shard_digest(dir(), router_, ShardId{0}, shard_count, RiskLinkPolicy::FailOpen);
    EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace lockstep::main_app
