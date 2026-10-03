#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <vector>

#include "lockstep/app/ports/journal.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/journal/format.hpp"

namespace lockstep::journal {

/// When commit() forces data to stable storage.
enum class SyncPolicy : std::uint8_t {
    None,         ///< write(2) only: survives a process crash, not a power loss
    EveryCommit,  ///< fdatasync(2) after every commit: survives both
};

/// Fsyncs `dir` itself (not its contents): makes durable the fact that a file
/// or subdirectory was just created inside it, which fdatasync on that file
/// does not cover. Returns 0 on success or an errno value. The composition
/// root uses this after creating a fresh journal directory (`main.cpp`);
/// `create()` below uses it after creating the journal file.
[[nodiscard]] int sync_directory(const std::filesystem::path& dir) noexcept;

/// File-backed write-ahead journal of one shard (ADR-0004, ADR-0012).
///
/// append() only encodes into an in-memory buffer; commit() writes the whole
/// batch with as few write(2) calls as possible and syncs per SyncPolicy.
/// Called only from the owning shard's thread, like every app::Journal.
///
/// After an I/O error the writer is poisoned: every later call returns
/// IoFailure, because the file may end in a partial record and appending
/// behind it would make the damage unrecoverable. (The runtime treats any
/// journal error as fatal anyway.)
///
/// Commands appended after the last commit() are discarded on destruction:
/// they were never committed, so their outputs were never released.
class FileJournalWriter final : public app::Journal {
public:
    /// Creates `<dir>/shard-<id>.jnl` and writes `header`. Throws
    /// std::runtime_error if `dir` does not exist or the file already exists
    /// (a startup error, ADR-0008; recovery of an existing journal is task 009).
    [[nodiscard]] static std::unique_ptr<FileJournalWriter> create(const std::filesystem::path& dir,
                                                                   const FileHeader& header,
                                                                   SyncPolicy policy);

    /// The file name of shard `shard`'s journal inside a journal directory.
    [[nodiscard]] static std::filesystem::path file_name(domain::ShardId shard);

    FileJournalWriter(const FileJournalWriter&) = delete;
    FileJournalWriter& operator=(const FileJournalWriter&) = delete;
    FileJournalWriter(FileJournalWriter&&) = delete;
    FileJournalWriter& operator=(FileJournalWriter&&) = delete;
    ~FileJournalWriter() override;

    [[nodiscard]] std::expected<void, app::JournalError> append(
        const domain::SequencedCommand& command) override;
    [[nodiscard]] std::expected<void, app::JournalError> commit() override;

private:
    FileJournalWriter(int fd, SyncPolicy policy) noexcept;

    [[nodiscard]] std::expected<void, app::JournalError> write_buffer() noexcept;

    int fd_;
    SyncPolicy policy_;
    bool failed_{false};
    std::vector<std::byte> buffer_;  // whole records (header + payload) since the last commit
};

}  // namespace lockstep::journal
