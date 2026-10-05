// compute_shard_digest (shard_digest.hpp), the from-disk oracle lockstep-
// replay and the determinism tests compare a live run's digest against
// (task 010 review M1/F1): a torn tail - including one on the very first
// record - is reported and treated as end of input (acceptance criterion
// 6, task 010 review m4), a file shorter than the journal header is an
// error, and so is any other journal error.
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

// Task 010 review F5: a file shorter than the header is an error, not a
// (vacuous) torn tail with nothing to report.
TEST_F(ShardDigestTest, FileShorterThanTheHeaderIsAnError) {
    fs::resize_file(path(), journal::file_header_size - 1);
    const auto result =
        compute_shard_digest(dir(), router_, ShardId{0}, shard_count, RiskLinkPolicy::FailOpen);
    EXPECT_FALSE(result.has_value());
}

// Task 010 review m4 (supersedes F5's TruncatedBeforeAnyRecordIsAnError): a
// torn *first* record is not special - it is the same "crash mid-write" a
// torn tail anywhere else is, and recover_tail (the path exchange-core's
// own startup takes) cuts it the same way. The file is at least
// header-sized here, so there is nothing wrong with the file itself, only
// with its last (here, only) record: benign, torn_tail=true, 0 commands.
TEST_F(ShardDigestTest, TornFirstRecordIsBenignWithZeroCommands) {
    fs::resize_file(path(), journal::file_header_size + 3);  // header + a few stray bytes
    const auto result =
        compute_shard_digest(dir(), router_, ShardId{0}, shard_count, RiskLinkPolicy::FailOpen);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->torn_tail);
    EXPECT_EQ(result->line.commands, 0U);
}

// Task 010 review F3: sequence numbers must start at 1 and increase by
// exactly 1.
TEST_F(ShardDigestTest, SequenceGapIsAnError) {
    const ShardConfig config{.shard = ShardId{0}, .instruments = instruments()};
    {
        // Overwrite with a journal whose second record jumps to sequence 3.
        fs::remove(path());
        const auto writer = journal::FileJournalWriter::create(
            dir(),
            journal::FileHeader{.shard = ShardId{0},
                                .shard_count = shard_count,
                                .config_hash = journal::config_hash(config)},
            journal::SyncPolicy::EveryCommit);
        const NewOrder order{.trader = TraderId{1},
                             .client_order_id = ClientOrderId{1},
                             .instrument = InstrumentId{1},
                             .price = Price{10},
                             .quantity = Quantity{1}};
        ASSERT_TRUE(writer->append(SequencedCommand{SequenceNumber{1}, Timestamp{1'000}, order})
                        .has_value());
        ASSERT_TRUE(writer->append(SequencedCommand{SequenceNumber{3}, Timestamp{1'001}, order})
                        .has_value());
        ASSERT_TRUE(writer->commit().has_value());
    }
    const auto result =
        compute_shard_digest(dir(), router_, ShardId{0}, shard_count, RiskLinkPolicy::FailOpen);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find('3'), std::string::npos) << result.error();
}

// Task 010 review F6: a stray journal for a shard id >= shard_count is
// refused, naming the file, by both exchange-core and lockstep-replay
// (validate_journal_dir is shared by their startup/pre-flight paths).
TEST_F(ShardDigestTest, ValidateRefusesAStrayFileForAShardIdAtOrAboveShardCount) {
    const ShardConfig config{.shard = ShardId{5}, .instruments = instruments()};
    const auto writer = journal::FileJournalWriter::create(
        dir(),
        journal::FileHeader{.shard = ShardId{5},
                            .shard_count = shard_count,
                            .config_hash = journal::config_hash(config)},
        journal::SyncPolicy::EveryCommit);

    const auto result = validate_journal_dir(dir(), shard_count);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("shard-5.jnl"), std::string::npos) << result.error();
}

TEST_F(ShardDigestTest, ValidateRefusesAPartialSetOfShardJournals) {
    // This fixture's dir() already has shard 0's journal; shard_count=3
    // asks for shards 0, 1 and 2, so 1 and 2 are missing.
    const auto result = validate_journal_dir(dir(), /*shard_count=*/3);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("shard-1.jnl"), std::string::npos) << result.error();
    EXPECT_NE(result.error().find("shard-2.jnl"), std::string::npos) << result.error();
}

TEST_F(ShardDigestTest, ValidateAcceptsACompleteSetOfShardJournals) {
    EXPECT_TRUE(validate_journal_dir(dir(), shard_count).has_value());
}

TEST_F(ShardDigestTest, ValidateAcceptsANonExistentDirectory) {
    EXPECT_TRUE(validate_journal_dir(dir() / "does-not-exist-yet", shard_count).has_value());
}

// Task 010 review m3: a file only counts as present if its name is exactly
// FileJournalWriter::file_name(id) - "shard-00.jnl" parses to shard 0, but
// is not the name create() or open_for_append() ever write, so treating it
// as shard 0's journal would silently accept a file nothing in this
// codebase produced.
TEST_F(ShardDigestTest, ValidateRefusesANonCanonicalFilename) {
    std::ofstream{dir() / "shard-00.jnl"} << "not a real journal";
    const auto result = validate_journal_dir(dir(), shard_count);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("shard-00.jnl"), std::string::npos) << result.error();
}

// Task 010 review m3: a shard-*.jnl file whose digits don't even parse as an
// unsigned integer must be refused, naming it, not silently ignored as "not
// ours to judge".
TEST_F(ShardDigestTest, ValidateRefusesAnUnparsableFilename) {
    std::ofstream{dir() / "shard-abc.jnl"} << "not a real journal";
    const auto result = validate_journal_dir(dir(), shard_count);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("shard-abc.jnl"), std::string::npos) << result.error();
}

// Task 010 review m3: digits that parse but overflow std::uint32_t must be
// refused explicitly, not left to from_chars' result_out_of_range being
// mistaken for "doesn't parse, ignore it".
TEST_F(ShardDigestTest, ValidateRefusesAnOverflowingShardId) {
    std::ofstream{dir() / "shard-99999999999.jnl"} << "not a real journal";
    const auto result = validate_journal_dir(dir(), shard_count);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("shard-99999999999.jnl"), std::string::npos) << result.error();
}

// Task 010 review m3: a directory_iterator error (e.g. a directory that
// becomes unreadable mid-listing) must be reported, not silently stop the
// listing early and report "looks fine" on whatever was seen before it.
TEST_F(ShardDigestTest, ValidateReportsADirectoryIteratorError) {
    // Root bypasses the DAC permission check this test relies on to make
    // directory_iterator fail, so there is nothing for it to catch here;
    // the production fix (returning the error instead of silently
    // `break`-ing out of the loop) is still exercised by any non-root run,
    // including CI.
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses the permission check this test needs";
    }
    // Not world/owner-readable: directory_iterator fails to open it.
    fs::permissions(dir(), fs::perms::none);
    const auto result = validate_journal_dir(dir(), shard_count);
    fs::permissions(dir(), fs::perms::owner_all);  // TearDown must still be able to remove it
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find(dir().string()), std::string::npos) << result.error();
}

}  // namespace
}  // namespace lockstep::main_app
