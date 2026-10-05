#include "lockstep/journal/file_journal_writer.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "lockstep/app/fatal.hpp"
#include "lockstep/journal/crc32c.hpp"
#include "lockstep/journal/record_codec.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace lockstep::journal {

namespace {

/// Writes all of `bytes`, retrying short writes and EINTR. Returns 0 or errno.
int write_all(int fd, std::span<const std::byte> bytes) noexcept {
    while (!bytes.empty()) {
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return 0;
}

int sync_data(int fd) noexcept {
    while (::fdatasync(fd) != 0) {
        if (errno != EINTR) {
            return errno;
        }
    }
    return 0;
}

[[noreturn]] void throw_io(const std::string& what, const std::filesystem::path& path, int error) {
    throw std::runtime_error("journal: " + what + " '" + path.string() +
                             "': " + std::generic_category().message(error));
}

}  // namespace

int sync_directory(const std::filesystem::path& dir) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic by POSIX
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return errno;
    }
    int error = 0;
    while (::fsync(fd) != 0) {
        if (errno != EINTR) {
            error = errno;
            break;
        }
    }
    (void)::close(fd);
    return error;
}

std::filesystem::path FileJournalWriter::file_name(domain::ShardId shard) {
    return "shard-" + std::to_string(shard.value()) + ".jnl";
}

std::unique_ptr<FileJournalWriter> FileJournalWriter::create(const std::filesystem::path& dir,
                                                             const FileHeader& header,
                                                             SyncPolicy policy) {
    const std::filesystem::path path = dir / file_name(header.shard);
    // O_EXCL: never truncate or append to an existing journal. Reopening one
    // needs recover_tail() of a possibly torn tail first (resuming is task 010).
    // Owner-only: the journal holds every trader's order flow.
    constexpr mode_t file_mode = S_IRUSR | S_IWUSR;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic by POSIX
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, file_mode);
    if (fd < 0) {
        throw_io("cannot create", path, errno);
    }
    // Owns fd from here on, so every throw below closes it.
    std::unique_ptr<FileJournalWriter> writer{new FileJournalWriter{fd, policy}};

    const auto encoded = encode(header);
    int error = write_all(fd, encoded);
    if (error == 0 && policy == SyncPolicy::EveryCommit) {
        error = sync_data(fd);
        if (error == 0) {
            error = sync_directory(dir);
        }
    }
    if (error != 0) {
        // Leave nothing behind that a restart would refuse (O_EXCL) or
        // mistake for a journal.
        writer.reset();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw_io("cannot write header of", path, error);
    }
    return writer;
}

std::unique_ptr<FileJournalWriter> FileJournalWriter::open_for_append(
    const std::filesystem::path& dir, domain::ShardId shard, SyncPolicy policy) {
    const std::filesystem::path path = dir / file_name(shard);
    // O_APPEND: every write(2) atomically seeks to the current end of file
    // first, so this writer never needs to track or restore a byte offset,
    // and cannot clobber the recovered tail even if something else sized the
    // file between recover_tail() and this open. No O_CREAT/O_EXCL: resuming
    // an existing, already-recovered file (ADR-0020), never creating one.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic by POSIX
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        throw_io("cannot reopen", path, errno);
    }
    return std::unique_ptr<FileJournalWriter>{new FileJournalWriter{fd, policy}};
}

FileJournalWriter::FileJournalWriter(int fd, SyncPolicy policy) noexcept
    : fd_{fd}, policy_{policy} {}

FileJournalWriter::~FileJournalWriter() {
    (void)::close(fd_);
}

std::expected<void, app::JournalError> FileJournalWriter::append(
    const domain::SequencedCommand& command) {
    if (failed_) {
        return std::unexpected(app::JournalError::IoFailure);
    }
    // Encode in place behind a placeholder record header, then fill the
    // header in: one buffer, no per-record allocation once it has grown.
    const std::size_t start = buffer_.size();
    buffer_.resize(start + record_header_size);
    encode_payload(command, buffer_);

    const auto payload = std::span{buffer_}.subspan(start + record_header_size);
    // Cannot happen with today's commands (the largest is a few dozen bytes):
    // a writer that produced one would be a programming bug, not journal
    // corruption to report through JournalError, so it is fatal like other
    // invariant breaches (ADR-0008).
    if (payload.size() > max_payload_size) {
        app::fatal("journal: encoded payload exceeds max_payload_size");
    }
    const RecordHeader header{.payload_size = static_cast<std::uint32_t>(payload.size()),
                              .crc32c = crc32c(payload)};
    const auto encoded = encode(header);
    std::ranges::copy(encoded, buffer_.begin() + static_cast<std::ptrdiff_t>(start));
    return {};
}

std::expected<void, app::JournalError> FileJournalWriter::commit() {
    if (failed_) {
        return std::unexpected(app::JournalError::IoFailure);
    }
    if (buffer_.empty()) {
        return {};
    }
    return write_buffer();
}

std::expected<void, app::JournalError> FileJournalWriter::write_buffer() noexcept {
    int error = write_all(fd_, buffer_);
    if (error == 0 && policy_ == SyncPolicy::EveryCommit) {
        error = sync_data(fd_);
    }
    if (error != 0) {
        failed_ = true;
        return std::unexpected(app::JournalError::IoFailure);
    }
    buffer_.clear();  // keeps capacity: steady state appends without allocating
    return {};
}

}  // namespace lockstep::journal
