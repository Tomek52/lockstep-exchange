#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::app {

enum class JournalError : std::uint8_t {
    IoFailure,
    Corrupt,          ///< bad magic, CRC mismatch, impossible length
    Truncated,        ///< torn write at the tail (recoverable: stop reading there)
    VersionMismatch,  ///< file written by an incompatible format version
    ConfigMismatch,   ///< shard id/count or shard config (ADR-0017) differs from the running one
};

[[nodiscard]] constexpr std::string_view to_string(JournalError error) noexcept {
    switch (error) {
        case JournalError::IoFailure:
            return "I/O failure";
        case JournalError::Corrupt:
            return "corrupt journal";
        case JournalError::Truncated:
            return "truncated journal";
        case JournalError::VersionMismatch:
            return "journal version mismatch";
        case JournalError::ConfigMismatch:
            return "journal config mismatch";
    }
    std::unreachable();
}

/// Outbound port: the write-ahead command journal of ONE shard (ADR-0004).
///
/// Called only from that shard's thread. The runtime calls append() for every
/// command before applying it, and commit() once per batch before releasing
/// the batch's events and replies. After commit() returns, the batch must be
/// durable according to the adapter's policy (buffered, fdatasync, ...).
///
/// A virtual interface rather than a template parameter: it is crossed once
/// per command/batch, where an indirect call is noise next to the I/O, and it
/// keeps the shard runtime a non-template (ADR-0002).
class Journal {
public:
    Journal() = default;
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&&) = delete;
    Journal& operator=(Journal&&) = delete;
    virtual ~Journal() = default;

    [[nodiscard]] virtual std::expected<void, JournalError> append(
        const domain::SequencedCommand& command) = 0;
    [[nodiscard]] virtual std::expected<void, JournalError> commit() = 0;
};

/// Creates the journal for a shard; chosen by the composition root.
using JournalFactory = std::move_only_function<std::unique_ptr<Journal>(domain::ShardId)>;

}  // namespace lockstep::app
