#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include "lockstep/app/ports/journal.hpp"
#include "lockstep/domain/commands.hpp"

namespace lockstep::journal {

/// Journal that keeps commands in memory. Used by the walking skeleton and by
/// determinism tests until the file journal lands (tasks 008/009), and later
/// as the fast journal for unit tests. Not durable.
class MemoryJournal final : public app::Journal {
public:
    [[nodiscard]] std::expected<void, app::JournalError> append(
        const domain::SequencedCommand& command) override {
        records_.push_back(command);
        return {};
    }

    [[nodiscard]] std::expected<void, app::JournalError> commit() override {
        committed_ = records_.size();
        return {};
    }

    /// Commands covered by the last commit(). Read only after the owning
    /// shard thread has been joined.
    [[nodiscard]] std::span<const domain::SequencedCommand> committed() const noexcept {
        return std::span{records_}.first(committed_);
    }

private:
    std::vector<domain::SequencedCommand> records_;
    std::size_t committed_{0};
};

/// Journal that discards everything: for benchmarks that must exclude I/O.
class NullJournal final : public app::Journal {
public:
    [[nodiscard]] std::expected<void, app::JournalError> append(
        const domain::SequencedCommand& /*command*/) override {
        return {};
    }
    [[nodiscard]] std::expected<void, app::JournalError> commit() override { return {}; }
};

}  // namespace lockstep::journal
