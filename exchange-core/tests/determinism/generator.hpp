#pragma once

// Shared command generator for the determinism test suite (task 010,
// acceptance criterion 1). A fixed seed given to random_command's rng
// reproduces the same command multiset except the target ids of cancels
// and modifies, which come from completions - themselves timing-dependent,
// since they only arrive once the publisher thread has delivered the
// matching accept (AcceptedOrders::sample, below, still draws exactly one
// rng value either way, so only the *result* varies with timing, not the
// rest of the command's shape). Several producers racing against a live,
// multi-threaded engine also interleave differently run to run; that is
// exactly the property ADR-0004 requires replay to reproduce regardless of
// interleaving.
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <vector>

#include "lockstep/app/engine.hpp"
#include "lockstep/app/messages.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/types.hpp"

#include "app/test_support.hpp"

namespace lockstep::test {

inline constexpr std::uint32_t instrument_count = 4;

[[nodiscard]] inline std::vector<domain::InstrumentSpec> instruments() {
    return std::views::iota(1U, instrument_count + 1) | std::views::transform([](std::uint32_t id) {
               return domain::InstrumentSpec{.id = domain::InstrumentId{id}};
           }) |
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
[[nodiscard]] domain::Command random_command(std::mt19937_64& rng,
                                             domain::TraderId trader,
                                             AcceptedOrders& accepted);

/// A BlockTrader/UnblockTrader/KillSwitch broadcast about one in
/// `risk_command_every` calls on average, nullopt otherwise. Every call
/// shares one process-wide RiskCommandId counter (idempotency keys must be
/// unique across every producer, not just within one).
inline constexpr int risk_command_every = 400;

[[nodiscard]] std::optional<domain::Command> maybe_risk_command(std::mt19937_64& rng,
                                                                domain::TraderId target_trader);

inline constexpr std::uint64_t default_producer_count = 3;
inline constexpr int default_commands_per_producer = 2'000;

/// Runs the shared randomized workload against an already-constructed,
/// not-yet-started `engine` (so the caller picks its journal factory: memory
/// for a fast in-process comparison, file for the on-disk round trip): adds
/// `subscriber`, starts the engine, races `producer_count` producers each
/// submitting `commands_per_producer` commands from random_command() plus
/// occasional risk broadcasts, appends every CommandReply to `live_replies`
/// (completions run on the publisher thread, so this is its only writer),
/// then stops the engine. After this returns, `engine`, `subscriber` and
/// `live_replies` hold the complete live run, and `engine`'s shards are
/// inspectable (ShardRuntime::engine()/book()) since Engine::stop() joined.
void run_workload(app::Engine& engine,
                  RecordingSubscriber& subscriber,
                  std::vector<app::CommandReply>& live_replies,
                  std::uint64_t producer_count = default_producer_count,
                  int commands_per_producer = default_commands_per_producer);

}  // namespace lockstep::test
