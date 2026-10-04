#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <generator>
#include <optional>
#include <span>

#include "lockstep/app/ports/journal.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::journal {

/// What the caller requires of a journal file. An unset field is not checked.
/// A mismatch yields JournalError::ConfigMismatch (ADR-0012, ADR-0017).
struct ReaderExpectations {
    std::optional<domain::ShardId> shard = std::nullopt;
    std::optional<std::uint32_t> shard_count = std::nullopt;
    std::optional<std::uint64_t> config_hash = std::nullopt;
};

/// Opens and validates the header of `path`, then yields its records lazily,
/// in file order, reading the file in bounded chunks (never whole).
///
/// After the first error the sequence yields that error once and finishes:
///  - IoFailure: the file cannot be opened or read;
///  - Truncated: the last record (or the header) runs past end-of-file, a
///    torn tail write (ADR-0012); every complete record was yielded before it;
///  - Corrupt: bad magic, impossible record length, CRC mismatch or an
///    undecodable payload. A CRC mismatch is Corrupt even in the last record:
///    only a record cut short by end-of-file is a torn write;
///  - VersionMismatch, ConfigMismatch: see the header check.
///
/// The error is in the sequence, not thrown, so the caller decides whether a
/// torn tail is acceptable (recovery) or fatal (replay of a sealed journal).
/// `path` and `expect` are copied; the file is closed when the generator is
/// destroyed or finished. Not thread-safe; use one generator per thread.
[[nodiscard]] std::generator<std::expected<domain::SequencedCommand, app::JournalError>>
read_journal(std::filesystem::path path, ReaderExpectations expect = {});

/// Truncates a journal whose tail is torn to the end of its last complete,
/// CRC-valid record, and syncs the file. Returns the number of valid records
/// (also when nothing had to be cut).
///
/// Never modifies a file it refuses: Corrupt, VersionMismatch, IoFailure, and
/// Truncated when the file ends inside the 32-byte header (no record was ever
/// written, and deleting the file is the caller's decision).
/// Must not run while a writer or another reader-recoverer has the file open.
[[nodiscard]] std::expected<std::uint64_t, app::JournalError> recover_tail(
    const std::filesystem::path& path);

/// One record decoded from the front of an in-memory buffer.
struct ParsedRecord {
    domain::SequencedCommand command;
    std::size_t size{0};  ///< bytes consumed: record header plus payload
};

/// Decodes the record at the front of `bytes`, with exactly the checks the
/// file reader applies: Truncated when the record header or payload extends
/// past the buffer (so an empty buffer is Truncated too; callers test for the
/// end themselves), Corrupt for a payload length above max_payload_size, a CRC
/// mismatch or an undecodable payload. Total over arbitrary bytes, which is
/// what the journal fuzzer relies on.
[[nodiscard]] std::expected<ParsedRecord, app::JournalError> parse_record(
    std::span<const std::byte> bytes) noexcept;

}  // namespace lockstep::journal
