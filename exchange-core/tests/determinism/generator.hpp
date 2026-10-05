#pragma once

// Shared command generator for the determinism test suite (task 010,
// acceptance criterion 1). A fixed seed given to random_command's rng
// reproduces the same command *multiset* from one producer; several
// producers racing against a live, multi-threaded engine still interleave
// differently run to run; that is exactly the property ADR-0004 requires
// replay to reproduce regardless of interleaving.
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <vector>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::test {

inline constexpr std::uint32_t instrument_count = 4;

[[nodiscard]] inline std::vector<domain::InstrumentSpec> instruments() {
    return std::views::iota(1U, instrument_count + 1) |
           std::views::transform(
               [](std::uint32_t id) { return domain::InstrumentSpec{.id = domain::InstrumentId{id}}; }) |
           std::ranges::to<std::vector>();
}

/// One producer's view of the order ids it has seen accepted, so it can
/// target cancels and modifies at real resting orders instead of only
/// random ids (acceptance criterion 1: "ids seen accepted"). Thread-safe:
/// record() runs wherever a completion is delivered (the publisher thread
/// in a live multi-threaded run), sample() on the producer thread that
/// generates the next command - a plain mutex is fine here, this is test
/// harness code, not the domain's single-writer hot path (ADR-0003 is about
/// ShardEngine, not test generators).
class AcceptedOrders {
public:
    void record(domain::OrderId id);
    [[nodiscard]] std::optional<domain::OrderId> sample(std::mt19937_64& rng) const;

private:
    mutable std::mutex mutex_;
    std::vector<domain::OrderId> ids_;
};

/// One command for `trader`, mixing (acceptance criterion 1):
///  - resting and crossing limit orders (a narrow price range makes crosses,
///    and therefore trades, frequent without special-casing them);
///  - market and IOC orders;
///  - cancels/modifies targeting `accepted`'s ids when it has any, an
///    unknown id otherwise (exercising RejectReason::UnknownOrder);
///  - occasional invalid price/quantity and duplicate client-order-id
///    submissions - rejections the journal must still record and replay
///    identically, which is exactly what makes them worth generating.
[[nodiscard]] domain::Command random_command(std::mt19937_64& rng, domain::TraderId trader,
                                             AcceptedOrders& accepted);

/// A BlockTrader/UnblockTrader/KillSwitch broadcast about one in
/// `risk_command_every` calls on average, nullopt otherwise. Every call
/// shares one process-wide RiskCommandId counter (idempotency keys must be
/// unique across every producer, not just within one).
inline constexpr int risk_command_every = 400;

[[nodiscard]] std::optional<domain::Command> maybe_risk_command(std::mt19937_64& rng,
                                                                 domain::TraderId target_trader);

}  // namespace lockstep::test
