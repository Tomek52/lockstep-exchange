#include "lockstep/journal/file_journal_reader.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <generator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "lockstep/journal/crc32c.hpp"
#include "lockstep/journal/format.hpp"
#include "lockstep/journal/record_codec.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace lockstep::journal {

namespace {

using app::JournalError;

// ---- Record checks shared by the file cursor and parse_record --------------
// One definition of "Corrupt" for both, so the fuzzed in-memory parser cannot
// drift from what replay actually does to a file.

/// A length above the maximum is corruption whether or not the payload fits in
/// the file: a torn write cuts a valid record, it does not invent a length.
[[nodiscard]] std::expected<RecordHeader, JournalError> check_record_header(
    const RecordHeader& record) noexcept {
    if (record.payload_size > max_payload_size) {
        return std::unexpected(JournalError::Corrupt);
    }
    return record;
}

/// `payload` is complete (payload_size bytes): its CRC and encoding must hold.
[[nodiscard]] std::expected<domain::SequencedCommand, JournalError> decode_record_payload(
    const RecordHeader& record, std::span<const std::byte> payload) noexcept {
    if (crc32c(payload) != record.crc32c) {
        return std::unexpected(JournalError::Corrupt);
    }
    return decode_payload(payload);
}

// ---- File access -----------------------------------------------------------

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept : fd_{fd} {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~FileDescriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
            fd_ = -1;
        }
    }

    int fd_;
};

/// Reads the records of one journal file front to back through a fixed-size
/// buffer, so memory and I/O stay bounded however long the journal is.
class RecordCursor {
public:
    /// Opens `path` and validates its header against `expect`.
    [[nodiscard]] static std::expected<RecordCursor, JournalError> open(
        const std::filesystem::path& path, const ReaderExpectations& expect) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic by POSIX
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(JournalError::IoFailure);
        }
        RecordCursor cursor{FileDescriptor{fd}};

        std::array<std::byte, file_header_size> raw{};
        const auto got = cursor.read_exact(raw);
        if (!got) {
            return std::unexpected(got.error());
        }
        // A file shorter than the header decodes as Truncated.
        const auto header = decode_file_header(std::span<const std::byte>{raw}.first(*got));
        if (!header) {
            return std::unexpected(header.error());
        }
        if ((expect.shard && *expect.shard != header->shard) ||
            (expect.shard_count && *expect.shard_count != header->shard_count) ||
            (expect.config_hash && *expect.config_hash != header->config_hash)) {
            return std::unexpected(JournalError::ConfigMismatch);
        }
        return cursor;
    }

    /// The next record, std::nullopt at a clean end-of-file (exactly on a
    /// record boundary). An error leaves valid_end() at the previous record.
    [[nodiscard]] std::expected<std::optional<domain::SequencedCommand>, JournalError> next() {
        std::array<std::byte, record_header_size> raw{};
        const auto header_bytes = read_exact(raw);
        if (!header_bytes) {
            return std::unexpected(header_bytes.error());
        }
        if (*header_bytes == 0) {
            return std::optional<domain::SequencedCommand>{};
        }
        if (*header_bytes < raw.size()) {
            return std::unexpected(JournalError::Truncated);
        }
        const auto record = check_record_header(decode_record_header(raw));
        if (!record) {
            return std::unexpected(record.error());
        }

        payload_.resize(record->payload_size);
        const auto payload_bytes = read_exact(payload_);
        if (!payload_bytes) {
            return std::unexpected(payload_bytes.error());
        }
        if (*payload_bytes < payload_.size()) {
            return std::unexpected(JournalError::Truncated);
        }
        auto command = decode_record_payload(*record, payload_);
        if (!command) {
            return std::unexpected(command.error());
        }
        valid_end_ += record_header_size + payload_.size();
        return std::optional<domain::SequencedCommand>{*command};
    }

    /// File offset just past the last record next() returned successfully.
    [[nodiscard]] std::uint64_t valid_end() const noexcept { return valid_end_; }

private:
    // Large enough to amortise read(2); the largest record is far smaller.
    static constexpr std::size_t chunk_size = std::size_t{64} * 1024;

    explicit RecordCursor(FileDescriptor fd) : fd_{std::move(fd)}, chunk_(chunk_size) {}

    /// Fills `out` from the file. Returns fewer bytes than asked only at
    /// end-of-file; the caller decides whether that is clean or torn.
    [[nodiscard]] std::expected<std::size_t, JournalError> read_exact(std::span<std::byte> out) {
        std::size_t copied = 0;
        while (copied < out.size()) {
            if (chunk_pos_ == chunk_end_) {
                const auto filled = refill();
                if (!filled) {
                    return std::unexpected(filled.error());
                }
                if (*filled == 0) {
                    break;
                }
            }
            const std::size_t count = std::min(out.size() - copied, chunk_end_ - chunk_pos_);
            std::copy_n(chunk_.begin() + static_cast<std::ptrdiff_t>(chunk_pos_), count,
                        out.begin() + static_cast<std::ptrdiff_t>(copied));
            chunk_pos_ += count;
            copied += count;
        }
        return copied;
    }

    [[nodiscard]] std::expected<std::size_t, JournalError> refill() {
        for (;;) {
            const ssize_t count = ::read(fd_.get(), chunk_.data(), chunk_.size());
            if (count >= 0) {
                chunk_pos_ = 0;
                chunk_end_ = static_cast<std::size_t>(count);
                return chunk_end_;
            }
            if (errno != EINTR) {
                return std::unexpected(JournalError::IoFailure);
            }
        }
    }

    FileDescriptor fd_;
    std::vector<std::byte> chunk_;
    std::size_t chunk_pos_{0};
    std::size_t chunk_end_{0};
    std::vector<std::byte> payload_;
    std::uint64_t valid_end_{file_header_size};
};

/// Cuts `path` to `size` bytes and syncs, so a crash right after recovery
/// cannot bring the torn tail back.
[[nodiscard]] std::expected<void, JournalError> truncate_durably(const std::filesystem::path& path,
                                                                 std::uint64_t size) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic by POSIX
    const FileDescriptor fd{::open(path.c_str(), O_WRONLY | O_CLOEXEC)};
    if (fd.get() < 0) {
        return std::unexpected(JournalError::IoFailure);
    }
    if (::ftruncate(fd.get(), static_cast<off_t>(size)) != 0) {
        return std::unexpected(JournalError::IoFailure);
    }
    while (::fdatasync(fd.get()) != 0) {
        if (errno != EINTR) {
            return std::unexpected(JournalError::IoFailure);
        }
    }
    return {};
}

}  // namespace

std::generator<std::expected<domain::SequencedCommand, JournalError>> read_journal(
    std::filesystem::path path, ReaderExpectations expect) {
    auto cursor = RecordCursor::open(path, expect);
    if (!cursor) {
        co_yield std::unexpected(cursor.error());
        co_return;
    }
    for (;;) {
        auto next = cursor->next();
        if (!next) {
            co_yield std::unexpected(next.error());
            co_return;
        }
        const std::optional<domain::SequencedCommand>& command = *next;
        if (!command) {
            co_return;
        }
        co_yield *command;
    }
}

std::expected<std::uint64_t, JournalError> recover_tail(const std::filesystem::path& path) {
    auto cursor = RecordCursor::open(path, {});
    if (!cursor) {
        return std::unexpected(cursor.error());
    }
    std::uint64_t records = 0;
    for (;;) {
        const auto next = cursor->next();
        if (!next) {
            if (next.error() != JournalError::Truncated) {
                return std::unexpected(next.error());
            }
            break;  // torn tail: cut it below
        }
        if (!next->has_value()) {
            return records;  // ended on a record boundary: nothing to cut
        }
        ++records;
    }
    if (const auto cut = truncate_durably(path, cursor->valid_end()); !cut) {
        return std::unexpected(cut.error());
    }
    return records;
}

std::expected<ParsedRecord, JournalError> parse_record(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < record_header_size) {
        return std::unexpected(JournalError::Truncated);
    }
    const auto record = check_record_header(
        decode_record_header(bytes.first<record_header_size>()));
    if (!record) {
        return std::unexpected(record.error());
    }
    const std::size_t total = record_header_size + record->payload_size;
    if (bytes.size() < total) {
        return std::unexpected(JournalError::Truncated);
    }
    const auto command = decode_record_payload(*record, bytes.subspan(record_header_size,
                                                                       record->payload_size));
    if (!command) {
        return std::unexpected(command.error());
    }
    return ParsedRecord{.command = *command, .size = total};
}

}  // namespace lockstep::journal
