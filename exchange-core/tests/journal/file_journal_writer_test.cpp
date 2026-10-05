#include "lockstep/journal/file_journal_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
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

#include <sys/resource.h>
#include <sys/stat.h>
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

/// Forces write(2) to fail with EFBIG once a file would grow past `limit`
/// bytes, without killing the process: SIGXFSZ, which write(2) raises by
/// default when the limit is hit, is ignored for the lifetime of this guard.
/// Deterministic and root-independent, unlike filesystem-permission tricks.
class ScopedFileSizeLimit {
public:
    explicit ScopedFileSizeLimit(rlim_t limit) {
        if (::getrlimit(RLIMIT_FSIZE, &old_limit_) != 0) {
            throw std::runtime_error("getrlimit(RLIMIT_FSIZE) failed");
        }
        struct rlimit new_limit = old_limit_;
        new_limit.rlim_cur = limit;
        if (::setrlimit(RLIMIT_FSIZE, &new_limit) != 0) {
            throw std::runtime_error("setrlimit(RLIMIT_FSIZE) failed");
        }
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        if (::sigaction(SIGXFSZ, &ignore, &old_action_) != 0) {
            (void)::setrlimit(RLIMIT_FSIZE, &old_limit_);
            throw std::runtime_error("sigaction(SIGXFSZ) failed");
        }
    }

    ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
    ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;
    ScopedFileSizeLimit(ScopedFileSizeLimit&&) = delete;
    ScopedFileSizeLimit& operator=(ScopedFileSizeLimit&&) = delete;

    ~ScopedFileSizeLimit() {
        (void)::sigaction(SIGXFSZ, &old_action_, nullptr);
        (void)::setrlimit(RLIMIT_FSIZE, &old_limit_);
    }

private:
    struct rlimit old_limit_{};
    struct sigaction old_action_{};
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

// Acceptance criterion 5 / ADR-0020: resuming a journal reopens the same
// file for appending, instead of refusing it like create() does.
TEST_F(FileJournalWriterTest, OpenForAppendWritesAfterExistingRecords) {
    {
        const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::EveryCommit);
        for (std::uint64_t i = 0; i < 5; ++i) {
            ASSERT_TRUE(writer->append(sequenced(i)).has_value());
        }
        ASSERT_TRUE(writer->commit().has_value());
    }
    const auto size_before_reopen = fs::file_size(dir() / "shard-1.jnl");

    {
        const auto writer =
            FileJournalWriter::open_for_append(dir(), header.shard, SyncPolicy::EveryCommit);
        for (std::uint64_t i = 5; i < 8; ++i) {
            ASSERT_TRUE(writer->append(sequenced(i)).has_value());
        }
        ASSERT_TRUE(writer->commit().has_value());
    }

    const auto contents = read_file(dir() / "shard-1.jnl");
    EXPECT_GT(contents.size(), size_before_reopen);
    const std::span<const std::byte> file{contents};
    EXPECT_EQ(decode_file_header(file).value(), header);

    std::size_t offset = file_header_size;
    for (std::uint64_t i = 0; i < 8; ++i) {
        ASSERT_LE(offset + record_header_size, file.size());
        const RecordHeader record =
            decode_record_header(file.subspan(offset).first<record_header_size>());
        offset += record_header_size;
        ASSERT_LE(offset + record.payload_size, file.size());
        const auto payload = file.subspan(offset, record.payload_size);
        EXPECT_EQ(decode_payload(payload).value(), sequenced(i)) << "record " << i;
        offset += record.payload_size;
    }
    EXPECT_EQ(offset, file.size());
}

TEST_F(FileJournalWriterTest, OpenForAppendThrowsWhenFileIsMissing) {
    EXPECT_THROW((void)FileJournalWriter::open_for_append(dir(), header.shard, SyncPolicy::None),
                 std::runtime_error);
}

// Task 010 review F8: open_for_append does not verify or restore the file's
// permission bits (the architecture rule for this layer excludes
// <sys/stat.h>/fstat - see open_for_append's doc comment) - it keeps
// whatever mode the file already has, even one changed outside the exchange
// after create() made it owner-only.
TEST_F(FileJournalWriterTest, OpenForAppendKeepsWhateverModeTheFileAlreadyHas) {
    {
        const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::None);
    }
    const auto path = dir() / "shard-1.jnl";
    ASSERT_EQ(::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP), 0);

    const auto writer = FileJournalWriter::open_for_append(dir(), header.shard, SyncPolicy::None);
    struct stat info{};
    ASSERT_EQ(::stat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777U, static_cast<unsigned>(S_IRUSR | S_IWUSR | S_IRGRP));

    ASSERT_EQ(::chmod(path.c_str(), S_IRUSR | S_IWUSR), 0);
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

TEST_F(FileJournalWriterTest, CreatesFileWithOwnerOnlyPermissions) {
    const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::None);
    struct stat info{};
    ASSERT_EQ(::stat((dir() / "shard-1.jnl").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777U, static_cast<unsigned>(S_IRUSR | S_IWUSR));
}

TEST_F(FileJournalWriterTest, HeaderWriteFailureLeavesNoFileBehind) {
    const auto path = dir() / "shard-1.jnl";
    const ScopedFileSizeLimit cap{0};  // the 32-byte header itself cannot fit
    EXPECT_THROW((void)FileJournalWriter::create(dir(), header, SyncPolicy::None),
                 std::runtime_error);
    EXPECT_FALSE(fs::exists(path));
}

// After an I/O error the writer is poisoned (file_journal_writer.hpp): every
// later append()/commit() returns IoFailure without touching the file again,
// because it may already end in a half-written record.
TEST_F(FileJournalWriterTest, IoFailurePoisonsWriterForAllLaterCalls) {
    const auto writer = FileJournalWriter::create(dir(), header, SyncPolicy::None);
    ASSERT_TRUE(writer->append(sequenced(0)).has_value());

    // Cap the file at its current (header-only) size: the pending commit's
    // write(2) must grow it, so it fails with EFBIG once the cap is in place.
    const rlim_t limit = fs::file_size(dir() / "shard-1.jnl");
    const ScopedFileSizeLimit cap{limit};

    const auto committed = writer->commit();
    ASSERT_FALSE(committed.has_value());
    EXPECT_EQ(committed.error(), app::JournalError::IoFailure);

    const auto appended = writer->append(sequenced(1));
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(appended.error(), app::JournalError::IoFailure);

    const auto committed_again = writer->commit();
    ASSERT_FALSE(committed_again.has_value());
    EXPECT_EQ(committed_again.error(), app::JournalError::IoFailure);
}

}  // namespace
}  // namespace lockstep::journal
