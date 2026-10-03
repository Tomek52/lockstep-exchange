#include "lockstep/journal/file_journal_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/journal/crc32c.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/format.hpp"
#include "lockstep/journal/record_codec.hpp"

#include <unistd.h>

#ifndef LOCKSTEP_JOURNAL_DUMP_PATH
#error \
    "LOCKSTEP_JOURNAL_DUMP_PATH must name the journal-dump executable (tests/journal/CMakeLists.txt)"
#endif

namespace lockstep::journal {
namespace {

using app::JournalError;
using namespace domain;
namespace fs = std::filesystem;

constexpr FileHeader header{.version = format_version,
                            .shard = ShardId{1},
                            .shard_count = 2,
                            .config_hash = 0xFEED'FACE'CAFE'BEEFULL};

// A mix of every command type, so each payload layout goes through the reader.
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

std::size_t record_size(std::uint64_t i) {
    std::vector<std::byte> payload;
    encode_payload(sequenced(i), payload);
    return record_header_size + payload.size();
}

/// Offset in the file of record `index` (0-based).
std::size_t record_offset(std::uint64_t index) {
    std::size_t offset = file_header_size;
    for (std::uint64_t i = 0; i < index; ++i) {
        offset += record_size(i);
    }
    return offset;
}

struct ReadResult {
    std::vector<SequencedCommand> commands;  // every record before the first error
    std::optional<JournalError> error;       // the first error
    std::size_t elements_after_error{0};     // must stay 0: an error ends the sequence
};

ReadResult read_all(const fs::path& path, const ReaderExpectations& expect = {}) {
    ReadResult result;
    for (const auto& element : read_journal(path, expect)) {
        if (result.error) {
            ++result.elements_after_error;
        } else if (element) {
            result.commands.push_back(*element);
        } else {
            result.error = element.error();
        }
    }
    return result;
}

std::vector<SequencedCommand> commands_0_to(std::uint64_t count) {
    std::vector<SequencedCommand> out;
    for (std::uint64_t i = 0; i < count; ++i) {
        out.push_back(sequenced(i));
    }
    return out;
}

class FileJournalReaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               ("lockstep-jrd-" + std::to_string(::getpid()) + "-" + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] fs::path path() const {
        return dir_ / FileJournalWriter::file_name(header.shard);
    }

    /// Writes `count` commands (sequenced(0..count)) to the shard file.
    void write_journal(std::uint64_t count) const {
        const auto writer = FileJournalWriter::create(dir_, header, SyncPolicy::None);
        constexpr std::uint64_t batch = 1'000;
        for (std::uint64_t i = 0; i < count; ++i) {
            ASSERT_TRUE(writer->append(sequenced(i)).has_value());
            if ((i + 1) % batch == 0) {
                ASSERT_TRUE(writer->commit().has_value());
            }
        }
        ASSERT_TRUE(writer->commit().has_value());
    }

    [[nodiscard]] std::vector<std::byte> read_bytes() const {
        std::ifstream in{path(), std::ios::binary};
        const std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
        std::vector<std::byte> out(chars.size());
        std::ranges::transform(chars, out.begin(),
                               [](char c) { return static_cast<std::byte>(c); });
        return out;
    }

    void write_bytes(std::span<const std::byte> bytes) const {
        std::ofstream out{path(), std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    void flip_byte(std::size_t offset) const {
        auto bytes = read_bytes();
        ASSERT_LT(offset, bytes.size());
        bytes[offset] ^= std::byte{0x01};
        write_bytes(bytes);
    }

    void truncate_to(std::size_t size) const { fs::resize_file(path(), size); }

private:
    fs::path dir_;
};

// ---- Acceptance criterion 1: round trip and laziness ---------------------------

TEST_F(FileJournalReaderTest, RoundTripsTenThousandCommandsInOrder) {
    write_journal(10'000);

    const ReadResult result = read_all(path());

    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.elements_after_error, 0U);
    ASSERT_EQ(result.commands.size(), 10'000U);
    EXPECT_EQ(result.commands, commands_0_to(10'000));
}

TEST_F(FileJournalReaderTest, EmptyJournalYieldsNothing) {
    write_journal(0);

    const ReadResult result = read_all(path());

    EXPECT_FALSE(result.error.has_value());
    EXPECT_TRUE(result.commands.empty());
}

/// Bytes this process has read(2), from /proc/self/io; nullopt where the
/// kernel does not provide it. Counting syscall bytes shows directly how much
/// of the file the reader pulled, which no timing margin can.
std::optional<std::uint64_t> bytes_read_by_this_process() {
    std::ifstream io{"/proc/self/io"};
    std::string key;
    std::uint64_t value = 0;
    while (io >> key >> value) {
        if (key == "rchar:") {
            return value;
        }
    }
    return std::nullopt;
}

TEST_F(FileJournalReaderTest, FirstElementOfAMillionRecordsDoesNotReadTheWholeFile) {
    constexpr std::uint64_t records = 1'000'000;
    constexpr std::uint64_t read_bound = 256 * 1024;
    write_journal(records);
    ASSERT_GT(fs::file_size(path()), 32 * read_bound);

    const auto before = bytes_read_by_this_process();
    if (!before) {
        GTEST_SKIP() << "/proc/self/io is not available on this kernel";
    }
    {
        auto journal = read_journal(path());
        auto it = journal.begin();
        ASSERT_NE(it, journal.end());
        const auto& first = *it;
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(*first, sequenced(0));
    }
    const auto after = bytes_read_by_this_process();
    ASSERT_TRUE(after.has_value());

    EXPECT_LE(*after - *before, read_bound);
}

// ---- Acceptance criterion 2: torn tail ----------------------------------------

TEST_F(FileJournalReaderTest, TornTailYieldsCompleteRecordsThenTruncatedOnce) {
    write_journal(10);
    const std::size_t full_size = fs::file_size(path());
    const std::size_t last_start = record_offset(9);
    ASSERT_EQ(full_size, last_start + record_size(9));

    // Every way of tearing the last record: inside its header and inside its payload.
    for (std::size_t cut = last_start + 1; cut < full_size; ++cut) {
        SCOPED_TRACE("file cut to " + std::to_string(cut) + " bytes");
        truncate_to(cut);

        const ReadResult result = read_all(path());

        EXPECT_EQ(result.commands, commands_0_to(9));
        EXPECT_EQ(result.error, JournalError::Truncated);
        EXPECT_EQ(result.elements_after_error, 0U);
    }
}

TEST_F(FileJournalReaderTest, RecoverTailCutsAtLastCompleteRecordAndReadingEndsCleanly) {
    write_journal(10);
    const std::size_t full_size = fs::file_size(path());
    truncate_to(full_size - 3);

    const auto recovered = recover_tail(path());

    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, 9U);
    EXPECT_EQ(fs::file_size(path()), record_offset(9));
    const ReadResult result = read_all(path());
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.commands, commands_0_to(9));
}

TEST_F(FileJournalReaderTest, RecoverTailRecoversEveryTearPointOfTheLastRecord) {
    write_journal(10);
    const auto original = read_bytes();
    const std::size_t last_start = record_offset(9);

    for (std::size_t cut = last_start; cut < original.size(); ++cut) {
        SCOPED_TRACE("file cut to " + std::to_string(cut) + " bytes");
        write_bytes(std::span{original}.first(cut));

        const auto recovered = recover_tail(path());

        ASSERT_TRUE(recovered.has_value());
        EXPECT_EQ(*recovered, 9U);
        EXPECT_EQ(fs::file_size(path()), last_start);
    }
}

TEST_F(FileJournalReaderTest, RecoverTailOnACleanJournalChangesNothing) {
    write_journal(10);
    const auto original = read_bytes();

    const auto recovered = recover_tail(path());

    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, 10U);
    EXPECT_EQ(read_bytes(), original);
}

TEST_F(FileJournalReaderTest, RecoverTailOnAHeaderOnlyJournalReturnsZero) {
    write_journal(0);

    EXPECT_EQ(recover_tail(path()), 0U);
    EXPECT_EQ(fs::file_size(path()), file_header_size);
}

TEST_F(FileJournalReaderTest, RecoveredJournalCanBeAppendedToByAHandWrittenRecord) {
    // After recovery the file ends on a record boundary, so the next record
    // starts where the format expects it (resuming writes is task 010).
    write_journal(10);
    truncate_to(fs::file_size(path()) - 1);
    ASSERT_EQ(recover_tail(path()), 9U);

    std::vector<std::byte> record(record_header_size);
    encode_payload(sequenced(9), record);
    const auto payload = std::span<const std::byte>{record}.subspan(record_header_size);
    const auto encoded_header =
        encode(RecordHeader{static_cast<std::uint32_t>(payload.size()), crc32c(payload)});
    std::ranges::copy(encoded_header, record.begin());
    auto bytes = read_bytes();
    bytes.insert(bytes.end(), record.begin(), record.end());
    write_bytes(bytes);

    const ReadResult result = read_all(path());
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.commands, commands_0_to(10));
}

// ---- Acceptance criterion 3: corruption ---------------------------------------

TEST_F(FileJournalReaderTest, FlippedPayloadByteInRecordFiveOfTenIsCorrupt) {
    write_journal(10);
    const std::size_t record_five = record_offset(4);
    flip_byte(record_five + record_header_size + 3);  // inside the payload

    const ReadResult result = read_all(path());

    EXPECT_EQ(result.commands, commands_0_to(4));
    EXPECT_EQ(result.error, JournalError::Corrupt);
    EXPECT_EQ(result.elements_after_error, 0U);
}

TEST_F(FileJournalReaderTest, RecoverTailRefusesCorruptionAndLeavesTheFileUnchanged) {
    write_journal(10);
    flip_byte(record_offset(4) + record_header_size + 3);
    const auto corrupted = read_bytes();

    const auto recovered = recover_tail(path());

    ASSERT_FALSE(recovered.has_value());
    EXPECT_EQ(recovered.error(), JournalError::Corrupt);
    EXPECT_EQ(read_bytes(), corrupted);
}

TEST_F(FileJournalReaderTest, FlippedCrcByteIsCorrupt) {
    write_journal(10);
    flip_byte(record_offset(2) + 4);  // first byte of the stored CRC

    const ReadResult result = read_all(path());

    EXPECT_EQ(result.commands, commands_0_to(2));
    EXPECT_EQ(result.error, JournalError::Corrupt);
}

TEST_F(FileJournalReaderTest, CrcMismatchInTheLastRecordIsCorruptNotTruncated) {
    // The record is complete, so the file was not cut short: silently dropping
    // it as a torn tail would hide damage.
    write_journal(10);
    flip_byte(fs::file_size(path()) - 1);

    const ReadResult result = read_all(path());

    EXPECT_EQ(result.commands, commands_0_to(9));
    EXPECT_EQ(result.error, JournalError::Corrupt);
    EXPECT_EQ(recover_tail(path()), std::unexpected(JournalError::Corrupt));
}

TEST_F(FileJournalReaderTest, PayloadLengthAboveTheMaximumIsCorruptEvenWhenItRunsPastEof) {
    write_journal(10);
    auto bytes = read_bytes();
    const std::size_t at = record_offset(3);
    store_le(max_payload_size + 1, std::span<std::byte, 4>{bytes.data() + at, 4});
    write_bytes(bytes);

    const ReadResult result = read_all(path());

    EXPECT_EQ(result.commands, commands_0_to(3));
    EXPECT_EQ(result.error, JournalError::Corrupt);
    EXPECT_EQ(recover_tail(path()), std::unexpected(JournalError::Corrupt));
}

TEST_F(FileJournalReaderTest, ZeroFilledTailIsCorrupt) {
    // What a crash on a filesystem that extends files before writing can leave.
    write_journal(3);
    auto bytes = read_bytes();
    bytes.resize(bytes.size() + 64, std::byte{0});
    write_bytes(bytes);

    const ReadResult result = read_all(path());

    EXPECT_EQ(result.commands, commands_0_to(3));
    EXPECT_EQ(result.error, JournalError::Corrupt);
}

TEST_F(FileJournalReaderTest, ValidCrcOverAnUndecodablePayloadIsCorrupt) {
    write_journal(3);
    auto bytes = read_bytes();
    const std::array<std::byte, 3> junk{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto record_header = encode(RecordHeader{junk.size(), crc32c(junk)});
    bytes.insert(bytes.end(), record_header.begin(), record_header.end());
    bytes.insert(bytes.end(), junk.begin(), junk.end());
    write_bytes(bytes);

    const ReadResult result = read_all(path());

    EXPECT_EQ(result.commands, commands_0_to(3));
    EXPECT_EQ(result.error, JournalError::Corrupt);
}

// ---- Acceptance criterion 4: header ------------------------------------------

TEST_F(FileJournalReaderTest, WrongMagicIsCorrupt) {
    write_journal(3);
    flip_byte(0);
    const auto damaged = read_bytes();

    const ReadResult result = read_all(path());

    EXPECT_TRUE(result.commands.empty());
    EXPECT_EQ(result.error, JournalError::Corrupt);
    EXPECT_EQ(result.elements_after_error, 0U);
    EXPECT_EQ(recover_tail(path()), std::unexpected(JournalError::Corrupt));
    EXPECT_EQ(read_bytes(), damaged);
}

TEST_F(FileJournalReaderTest, NextVersionIsAVersionMismatch) {
    write_journal(3);
    auto bytes = read_bytes();
    store_le(static_cast<std::uint16_t>(format_version + 1), std::span<std::byte, 2>{&bytes[8], 2});
    write_bytes(bytes);

    const ReadResult result = read_all(path());

    EXPECT_TRUE(result.commands.empty());
    EXPECT_EQ(result.error, JournalError::VersionMismatch);
    EXPECT_EQ(recover_tail(path()), std::unexpected(JournalError::VersionMismatch));
    EXPECT_EQ(read_bytes(), bytes);
}

TEST_F(FileJournalReaderTest, ExpectationsThatMatchTheHeaderAreAccepted) {
    write_journal(3);

    const ReadResult result =
        read_all(path(), ReaderExpectations{.shard = header.shard,
                                            .shard_count = header.shard_count,
                                            .config_hash = header.config_hash});

    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.commands, commands_0_to(3));
}

TEST_F(FileJournalReaderTest, EachExpectationMismatchIsAConfigMismatch) {
    write_journal(3);
    const std::array<ReaderExpectations, 3> mismatches{
        ReaderExpectations{.shard = ShardId{header.shard.value() + 1}},
        ReaderExpectations{.shard_count = header.shard_count + 1},
        ReaderExpectations{.config_hash = header.config_hash + 1},
    };

    for (const ReaderExpectations& expect : mismatches) {
        const ReadResult result = read_all(path(), expect);

        EXPECT_TRUE(result.commands.empty());
        EXPECT_EQ(result.error, JournalError::ConfigMismatch);
        EXPECT_EQ(result.elements_after_error, 0U);
    }
}

TEST_F(FileJournalReaderTest, FileShorterThanTheHeaderIsTruncatedAndLeftAlone) {
    write_journal(0);
    truncate_to(file_header_size - 1);

    const ReadResult result = read_all(path());

    EXPECT_TRUE(result.commands.empty());
    EXPECT_EQ(result.error, JournalError::Truncated);
    EXPECT_EQ(recover_tail(path()), std::unexpected(JournalError::Truncated));
    EXPECT_EQ(fs::file_size(path()), file_header_size - 1);
}

TEST_F(FileJournalReaderTest, EmptyFileIsTruncated) {
    write_bytes({});

    EXPECT_EQ(read_all(path()).error, JournalError::Truncated);
}

TEST_F(FileJournalReaderTest, MissingFileIsAnIoFailure) {
    const ReadResult result = read_all(path());

    EXPECT_TRUE(result.commands.empty());
    EXPECT_EQ(result.error, JournalError::IoFailure);
    EXPECT_EQ(result.elements_after_error, 0U);
    EXPECT_EQ(recover_tail(path()), std::unexpected(JournalError::IoFailure));
}

// ---- parse_record: the in-memory parser the fuzzer drives ----------------------

TEST_F(FileJournalReaderTest, ParseRecordWalksAWholeFileBody) {
    write_journal(50);
    const auto bytes = read_bytes();
    std::span<const std::byte> rest{bytes};
    rest = rest.subspan(file_header_size);

    std::vector<SequencedCommand> parsed;
    while (!rest.empty()) {
        const auto record = parse_record(rest);
        ASSERT_TRUE(record.has_value());
        parsed.push_back(record->command);
        rest = rest.subspan(record->size);
    }

    EXPECT_EQ(parsed, commands_0_to(50));
}

TEST_F(FileJournalReaderTest, ParseRecordClassifiesEveryPrefixOfARecordAsTruncated) {
    write_journal(1);
    const auto bytes = read_bytes();
    const auto record = std::span{bytes}.subspan(file_header_size);

    for (std::size_t length = 0; length < record.size(); ++length) {
        SCOPED_TRACE("prefix of " + std::to_string(length) + " bytes");
        EXPECT_EQ(parse_record(record.first(length)), std::unexpected(JournalError::Truncated));
    }
    const auto whole = parse_record(record);
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->size, record.size());
}

// ---- Acceptance criterion 6: journal-dump ------------------------------------

std::vector<std::string> run_journal_dump(const fs::path& file, int& exit_code) {
    const std::string command =
        std::string{"\""} + LOCKSTEP_JOURNAL_DUMP_PATH + "\" \"" + file.string() + "\" 2>&1";
    std::vector<std::string> lines;
    // NOLINTNEXTLINE(cert-env33-c): the command is built from paths this test owns
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        exit_code = -1;
        return lines;
    }
    std::string current;
    std::array<char, 4096> chunk{};
    while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe) != nullptr) {
        current += chunk.data();
        if (current.ends_with('\n')) {
            current.pop_back();
            lines.push_back(std::move(current));
            current.clear();
        }
    }
    exit_code = ::pclose(pipe);
    return lines;
}

TEST_F(FileJournalReaderTest, JournalDumpPrintsAHeaderLineAndOneLinePerRecord) {
    write_journal(10'000);

    int exit_code = -1;
    const auto lines = run_journal_dump(path(), exit_code);

    EXPECT_EQ(exit_code, 0);
    ASSERT_EQ(lines.size(), 10'001U);
    EXPECT_EQ(lines[0], "journal version=1 shard=1 shard_count=2 config_hash=0xfeedfacecafebeef");
    EXPECT_EQ(lines[1],
              "seq=1 ts=1000000 NewOrder trader=0 instrument=2 Buy Limit 100@1 Gtc "
              "client_order_id=0");
    EXPECT_EQ(lines[2], "seq=2 ts=1000001 CancelOrder trader=1 instrument=2 order_id=1");
    EXPECT_EQ(lines[3],
              "seq=3 ts=1000002 ModifyOrder trader=2 instrument=2 order_id=2 price=5 quantity=6");
    EXPECT_EQ(lines[4], "seq=4 ts=1000003 BlockTrader command_id=3 trader=3");
    EXPECT_EQ(lines[5], "seq=5 ts=1000004 UnblockTrader command_id=4 trader=4");
    EXPECT_EQ(lines[6], "seq=6 ts=1000005 KillSwitch command_id=5 engaged=false");
    EXPECT_EQ(lines[7], "seq=7 ts=1000006 RiskLinkStatus connected=true");
}

TEST_F(FileJournalReaderTest, JournalDumpFailsWithAMessageOnACorruptJournal) {
    write_journal(10);
    flip_byte(record_offset(4) + record_header_size + 3);

    int exit_code = 0;
    const auto lines = run_journal_dump(path(), exit_code);

    EXPECT_NE(exit_code, 0);
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.back().find("corrupt journal"), std::string::npos) << lines.back();
}

}  // namespace
}  // namespace lockstep::journal
