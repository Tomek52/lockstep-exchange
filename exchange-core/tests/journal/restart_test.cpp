// recover_for_restart (ADR-0020): recover_tail + header-validated read, used
// by exchange-core's startup to resume a journal that already held commands
// instead of failing on O_EXCL.
#include "lockstep/journal/restart.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/record_codec.hpp"

#include <unistd.h>

namespace lockstep::journal {
namespace {

using namespace domain;
namespace fs = std::filesystem;

constexpr std::uint32_t shard_count = 2;

ShardConfig config_for(ShardId shard) {
    return ShardConfig{.shard = shard,
                       .instruments = {InstrumentSpec{.id = InstrumentId{1}},
                                       InstrumentSpec{.id = InstrumentId{2}}}};
}

SequencedCommand sequenced(std::uint64_t i) {
    return SequencedCommand{
        SequenceNumber{i + 1}, Timestamp{static_cast<std::int64_t>(1'000 + i)},
        NewOrder{.trader = TraderId{i},
                .client_order_id = ClientOrderId{i},
                .instrument = InstrumentId{1},
                .price = Price{static_cast<std::int64_t>(10 + i)},
                .quantity = Quantity{1}}};
}

class RestartTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-restart-" + std::to_string(::getpid()) + "-" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] const fs::path& dir() const { return dir_; }

    [[nodiscard]] std::vector<std::byte> read_file(const fs::path& path) const {
        std::ifstream in{path, std::ios::binary};
        const std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
        std::vector<std::byte> out(chars.size());
        std::ranges::transform(chars, out.begin(),
                               [](char c) { return static_cast<std::byte>(c); });
        return out;
    }

private:
    fs::path dir_;
};

void write_journal(const fs::path& dir, const ShardConfig& config, std::uint64_t commands) {
    const auto writer = FileJournalWriter::create(
        dir,
        FileHeader{.shard = config.shard, .shard_count = shard_count,
                  .config_hash = config_hash(config)},
        SyncPolicy::EveryCommit);
    for (std::uint64_t i = 0; i < commands; ++i) {
        ASSERT_TRUE(writer->append(sequenced(i)).has_value());
    }
    ASSERT_TRUE(writer->commit().has_value());
}

TEST_F(RestartTest, NoExistingFileIsAFreshStart) {
    const auto result = recover_for_restart(dir(), config_for(ShardId{0}), shard_count);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->existed);
    EXPECT_TRUE(result->resume_commands.empty());
}

TEST_F(RestartTest, ExistingCleanJournalYieldsEveryRecordInOrder) {
    const auto config = config_for(ShardId{0});
    write_journal(dir(), config, 5);

    const auto result = recover_for_restart(dir(), config, shard_count);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->existed);
    ASSERT_EQ(result->resume_commands.size(), 5U);
    for (std::uint64_t i = 0; i < 5; ++i) {
        EXPECT_EQ(result->resume_commands[i], sequenced(i)) << "record " << i;
    }
}

TEST_F(RestartTest, TornTailIsRecoveredAndOnlyCompleteRecordsReturn) {
    const auto config = config_for(ShardId{0});
    write_journal(dir(), config, 5);
    const auto path = dir() / FileJournalWriter::file_name(ShardId{0});
    // Simulate a crash mid-write: append 3 garbage bytes after the last
    // complete record, too short to be a valid record header.
    {
        std::ofstream out{path, std::ios::binary | std::ios::app};
        const std::array<char, 3> garbage{'\x01', '\x02', '\x03'};
        out.write(garbage.data(), garbage.size());
    }
    const auto size_before = fs::file_size(path);

    const auto result = recover_for_restart(dir(), config, shard_count);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->existed);
    EXPECT_EQ(result->resume_commands.size(), 5U);
    EXPECT_LT(fs::file_size(path), size_before) << "torn tail must be cut";
}

TEST_F(RestartTest, ConfigMismatchIsRefusedAndFileIsUnchanged) {
    const auto original = config_for(ShardId{0});
    write_journal(dir(), original, 3);
    const auto path = dir() / FileJournalWriter::file_name(ShardId{0});
    const auto before = read_file(path);

    // Different instrument set -> different config_hash (ADR-0017).
    ShardConfig different = original;
    different.instruments.push_back(InstrumentSpec{.id = InstrumentId{3}});
    const auto result = recover_for_restart(dir(), different, shard_count);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find(path.string()), std::string::npos)
        << "error must name the file: " << result.error();
    EXPECT_EQ(read_file(path), before);
}

// Acceptance criterion 5: a corrupt journal stops the restart outright and
// the file is byte-identical afterwards.
TEST_F(RestartTest, CorruptJournalIsRefusedAndFileIsByteIdentical) {
    const auto config = config_for(ShardId{0});
    write_journal(dir(), config, 4);
    const auto path = dir() / FileJournalWriter::file_name(ShardId{0});
    // Flip a payload byte well before the tail: a CRC mismatch there is
    // Corrupt, not a torn tail (file_journal_reader.hpp's contract).
    {
        std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(static_cast<std::streamoff>(file_header_size + record_header_size));
        char byte = 0;
        file.read(&byte, 1);
        file.seekp(static_cast<std::streamoff>(file_header_size + record_header_size));
        byte = static_cast<char>(~byte);
        file.write(&byte, 1);
    }
    const auto before = read_file(path);

    const auto result = recover_for_restart(dir(), config, shard_count);

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find(path.string()), std::string::npos)
        << "error must name the file: " << result.error();
    EXPECT_EQ(read_file(path), before) << "a refused journal must never be modified";
}

}  // namespace
}  // namespace lockstep::journal
