#include "lockstep/journal/file_journal_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/journal/crc32c.hpp"
#include "lockstep/journal/format.hpp"
#include "lockstep/journal/record_codec.hpp"

#include <unistd.h>

namespace lockstep::journal {
namespace {

using namespace domain;
namespace fs = std::filesystem;

class FileJournalWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-jnl-" + std::to_string(::getpid()) + "-" + info->name());
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

constexpr FileHeader header{.version = format_version,
                            .shard = ShardId{1},
                            .shard_count = 2,
                            .config_hash = 0xFEED'FACE'CAFE'BEEFULL};

Command command_number(std::uint64_t i) {
    switch (i % 7) {
        case 0:
            return NewOrder{.trader = TraderId{i},
                            .client_order_id = ClientOrderId{i * 3},
                            .instrument = InstrumentId{2},
                            .side = i % 2 == 0 ? Side::Buy : Side::Sell,
                            .price = Price{static_cast<std::int64_t>(100 + i)},
                            .quantity = Quantity{i + 1}};
        case 1:
            return CancelOrder{TraderId{i}, InstrumentId{2}, OrderId{i}};
        case 2:
            return ModifyOrder{TraderId{i}, InstrumentId{2}, OrderId{i}, Price{5}, Quantity{6}};
        case 3:
            return BlockTrader{RiskCommandId{i}, TraderId{i}};
        case 4:
            return UnblockTrader{RiskCommandId{i}, TraderId{i}};
        case 5:
            return KillSwitch{RiskCommandId{i}, i % 2 == 0};
        default:
            return RiskLinkStatus{i % 2 == 0};
    }
}

SequencedCommand sequenced(std::uint64_t i) {
    return SequencedCommand{SequenceNumber{i + 1},
                            Timestamp{static_cast<std::int64_t>(1'000'000 + i)}, command_number(i)};
}

TEST_F(FileJournalWriterTest, CreatesShardFileWithHeaderOnly) {
    const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::EveryCommit);
    const auto path = dir() / "shard-1.jnl";
    ASSERT_TRUE(fs::exists(path));
    const auto contents = read_file(path);
    ASSERT_EQ(contents.size(), file_header_size);
    EXPECT_EQ(decode_file_header(contents).value(), header);
}

// Acceptance criterion 5.
TEST_F(FileJournalWriterTest, ThousandCommandsInTenCommits) {
    constexpr std::uint64_t commands = 1'000;
    constexpr std::uint64_t commits = 10;
    std::size_t expected_size = file_header_size;
    {
        const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::EveryCommit);
        for (std::uint64_t i = 0; i < commands; ++i) {
            ASSERT_TRUE(writer->append(sequenced(i)).has_value());
            std::vector<std::byte> payload;
            encode_payload(sequenced(i), payload);
            expected_size += record_header_size + payload.size();
            if ((i + 1) % (commands / commits) == 0) {
                ASSERT_TRUE(writer->commit().has_value());
            }
        }
    }

    const auto contents = read_file(dir() / "shard-1.jnl");
    ASSERT_EQ(contents.size(), expected_size);
    const std::span<const std::byte> file{contents};
    EXPECT_EQ(decode_file_header(file).value(), header);

    std::size_t offset = file_header_size;
    for (std::uint64_t i = 0; i < commands; ++i) {
        ASSERT_LE(offset + record_header_size, file.size());
        const RecordHeader record =
            decode_record_header(file.subspan(offset).first<record_header_size>());
        offset += record_header_size;
        ASSERT_LE(offset + record.payload_size, file.size());
        const auto payload = file.subspan(offset, record.payload_size);
        EXPECT_EQ(record.crc32c, crc32c(payload)) << "record " << i;
        EXPECT_EQ(decode_payload(payload).value(), sequenced(i)) << "record " << i;
        offset += record.payload_size;
    }
    EXPECT_EQ(offset, file.size());
}

TEST_F(FileJournalWriterTest, AppendDoesNoIoUntilCommit) {
    const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::None);
    for (std::uint64_t i = 0; i < 20; ++i) {
        ASSERT_TRUE(writer->append(sequenced(i)).has_value());
    }
    const auto path = dir() / "shard-1.jnl";
    EXPECT_EQ(fs::file_size(path), file_header_size);
    ASSERT_TRUE(writer->commit().has_value());
    EXPECT_GT(fs::file_size(path), file_header_size);
}

TEST_F(FileJournalWriterTest, EmptyCommitWritesNothing) {
    const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::EveryCommit);
    ASSERT_TRUE(writer->commit().has_value());
    EXPECT_EQ(fs::file_size(dir() / "shard-1.jnl"), file_header_size);
}

TEST_F(FileJournalWriterTest, RefusesToOverwriteAnExistingJournal) {
    {
        (void)FileJournalWriter::create(dir(), header, SyncPolicy::None);
    }
    EXPECT_THROW((void)FileJournalWriter::create(dir(), header, SyncPolicy::None),
                 std::runtime_error);
    EXPECT_EQ(fs::file_size(dir() / "shard-1.jnl"), file_header_size);
}

TEST_F(FileJournalWriterTest, ThrowsWhenDirectoryIsMissing) {
    EXPECT_THROW((void)FileJournalWriter::create(dir() / "missing", header, SyncPolicy::None),
                 std::runtime_error);
}

}  // namespace
}  // namespace lockstep::journal
