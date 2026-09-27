#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/flat_map.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::app {

/// Static instrument -> shard assignment. Fixed for the lifetime of a journal:
/// the journal header records it and replay refuses a different one (ADR-0003).
class Router {
public:
    /// Sorts instruments by id and deals them round-robin across shards, so the
    /// assignment depends only on the instrument set, not on config order.
    [[nodiscard]] static Router round_robin(std::span<const domain::InstrumentSpec> instruments,
                                            std::size_t shard_count);

    [[nodiscard]] std::optional<domain::ShardId> shard_for(domain::InstrumentId instrument) const;
    [[nodiscard]] std::size_t shard_count() const noexcept { return by_shard_.size(); }
    [[nodiscard]] std::span<const domain::InstrumentSpec> instruments_of(
        domain::ShardId shard) const;

private:
    domain::flat_map<domain::InstrumentId, domain::ShardId> shard_of_;
    std::vector<std::vector<domain::InstrumentSpec>> by_shard_;
};

/// The instrument an order command targets; nullopt for risk commands.
[[nodiscard]] std::optional<domain::InstrumentId> instrument_of(
    const domain::Command& command) noexcept;

}  // namespace lockstep::app
