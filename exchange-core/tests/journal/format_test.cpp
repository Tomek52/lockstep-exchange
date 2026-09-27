#include "lockstep/journal/format.hpp"

#include <array>
#include <cstddef>

#include <gtest/gtest.h>

namespace lockstep::journal {
namespace {

constexpr FileHeader sample{.version = format_version,
                            .shard = domain::ShardId{3},
                            .shard_count = 4,
                            .config_hash = 0x0123'4567'89AB'CDEFULL};

// Encoding and decoding are constexpr (if consteval in byte_io.hpp), so the
// format's round-trip property is checked by the compiler.
static_assert(decode_file_header(encode(sample)).value() == sample);
static_assert(decode_record_header(encode(RecordHeader{17, 0xDEADBEEF})) ==
              RecordHeader{17, 0xDEADBEEF});
static_assert(encode(sample).size() == file_header_size);

TEST(JournalFormat, HeaderStartsWithMagicAndIsLittleEndian) {
    const auto bytes = encode(sample);
    EXPECT_EQ(bytes[0], std::byte{'L'});
    EXPECT_EQ(bytes[7], std::byte{'L'});
    EXPECT_EQ(bytes[12], std::byte{3});  // shard id, least significant byte first
    EXPECT_EQ(bytes[24], std::byte{0xEF});
}

TEST(JournalFormat, RejectsBadMagic) {
    auto bytes = encode(sample);
    bytes[0] = std::byte{'X'};
    EXPECT_EQ(decode_file_header(bytes).error(), app::JournalError::Corrupt);
}

TEST(JournalFormat, RejectsOtherVersions) {
    FileHeader future = sample;
    future.version = format_version + 1;
    EXPECT_EQ(decode_file_header(encode(future)).error(), app::JournalError::VersionMismatch);
}

TEST(JournalFormat, ShortInputIsTruncated) {
    const auto bytes = encode(sample);
    EXPECT_EQ(decode_file_header(std::span{bytes}.first(10)).error(), app::JournalError::Truncated);
}

}  // namespace
}  // namespace lockstep::journal
