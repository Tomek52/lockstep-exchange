#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "lockstep/app/ports/journal.hpp"
#include "lockstep/domain/types.hpp"
#include "lockstep/journal/byte_io.hpp"

namespace lockstep::journal {

// On-disk layout of a shard journal, format version 1 (ADR-0012).
//
//   file   := FileHeader Record*
//   Record := RecordHeader payload[payload_size]
//
//   FileHeader (32 bytes, little-endian)
//     0  magic          8  "LKSTPJNL"
//     8  version        2
//    10  reserved       2  (zero)
//    12  shard_id       4
//    16  shard_count    4
//    20  reserved       4  (zero)
//    24  config_hash    8  hash of the shard config (ADR-0017); replay refuses a mismatch
//
//   RecordHeader (8 bytes)
//     0  payload_size   4
//     4  crc32c         4  of the payload
//
//   payload: SequencedCommand encoding, specified in record_codec.hpp
//   (task 008).
//
// A record whose header or payload runs past end-of-file is a torn tail write
// (Truncated): readers stop there. A CRC mismatch anywhere else is Corrupt.

inline constexpr std::array<std::byte, 8> file_magic{std::byte{'L'}, std::byte{'K'}, std::byte{'S'},
                                                     std::byte{'T'}, std::byte{'P'}, std::byte{'J'},
                                                     std::byte{'N'}, std::byte{'L'}};
inline constexpr std::uint16_t format_version = 1;
inline constexpr std::size_t file_header_size = 32;
inline constexpr std::size_t record_header_size = 8;
/// Upper bound on a payload; anything larger is corruption, not a command.
inline constexpr std::uint32_t max_payload_size = 4096;

struct FileHeader {
    std::uint16_t version{format_version};
    domain::ShardId shard;
    std::uint32_t shard_count{1};
    std::uint64_t config_hash{0};

    friend constexpr bool operator==(const FileHeader&, const FileHeader&) = default;
};

struct RecordHeader {
    std::uint32_t payload_size{0};
    std::uint32_t crc32c{0};

    friend constexpr bool operator==(const RecordHeader&, const RecordHeader&) = default;
};

[[nodiscard]] constexpr std::array<std::byte, file_header_size> encode(
    const FileHeader& header) noexcept {
    std::array<std::byte, file_header_size> out{};
    const std::span bytes{out};
    for (std::size_t i = 0; i < file_magic.size(); ++i) {
        out[i] = file_magic[i];
    }
    store_le(header.version, bytes.subspan<8, 2>());
    store_le(header.shard.value(), bytes.subspan<12, 4>());
    store_le(header.shard_count, bytes.subspan<16, 4>());
    store_le(header.config_hash, bytes.subspan<24, 8>());
    return out;
}

[[nodiscard]] constexpr std::expected<FileHeader, app::JournalError> decode_file_header(
    std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < file_header_size) {
        return std::unexpected(app::JournalError::Truncated);
    }
    for (std::size_t i = 0; i < file_magic.size(); ++i) {
        if (bytes[i] != file_magic[i]) {
            return std::unexpected(app::JournalError::Corrupt);
        }
    }
    const auto fixed = bytes.first<file_header_size>();
    FileHeader header{
        .version = load_le<std::uint16_t>(fixed.subspan<8, 2>()),
        .shard = domain::ShardId{load_le<std::uint32_t>(fixed.subspan<12, 4>())},
        .shard_count = load_le<std::uint32_t>(fixed.subspan<16, 4>()),
        .config_hash = load_le<std::uint64_t>(fixed.subspan<24, 8>()),
    };
    if (header.version != format_version) {
        return std::unexpected(app::JournalError::VersionMismatch);
    }
    return header;
}

[[nodiscard]] constexpr std::array<std::byte, record_header_size> encode(
    const RecordHeader& header) noexcept {
    std::array<std::byte, record_header_size> out{};
    const std::span bytes{out};
    store_le(header.payload_size, bytes.subspan<0, 4>());
    store_le(header.crc32c, bytes.subspan<4, 4>());
    return out;
}

[[nodiscard]] constexpr RecordHeader decode_record_header(
    std::span<const std::byte, record_header_size> bytes) noexcept {
    return RecordHeader{load_le<std::uint32_t>(bytes.subspan<0, 4>()),
                        load_le<std::uint32_t>(bytes.subspan<4, 4>())};
}

}  // namespace lockstep::journal
