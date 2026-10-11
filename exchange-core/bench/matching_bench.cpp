// Matching engine latency (domain only, single thread, no I/O).
//
// Each command is timed individually and p50/p99/p99.9 are reported as
// counters. The timer itself (two steady_clock reads) is included in every
// sample and is not subtracted: an empty sample measured about 13 ns at p50
// on the machine in the README. Read the numbers as "the command plus a small
// constant", not as cycle-accurate figures.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <variant>
#include <vector>

#include <benchmark/benchmark.h>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/events.hpp"
#include "lockstep/domain/shard_engine.hpp"
#include "lockstep/domain/types.hpp"

#include "percentiles.hpp"

namespace {

using namespace lockstep::domain;

constexpr InstrumentId instrument_one{1};
constexpr TraderId trader{1};

ShardConfig single_shard(std::uint32_t instruments) {
    ShardConfig config{.shard = ShardId{0}, .instruments = {}};
    for (std::uint32_t id = 1; id <= instruments; ++id) {
        config.instruments.push_back(InstrumentSpec{.id = InstrumentId{id}});
    }
    return config;
}

/// Hands out strictly increasing sequence numbers and timestamps, like the
/// shard runtime does, and applies commands to one engine.
class Driver {
public:
    explicit Driver(std::uint32_t instruments) : engine_{single_shard(instruments)} {}

    /// Applies `command`. The outcome is returned so a benchmark can verify it
    /// and, for NewOrder, learn the assigned id.
    CommandResult apply(const Command& command) {
        events_.clear();
        ++sequence_;
        return engine_.apply(SequencedCommand{.sequence = SequenceNumber{sequence_},
                                              .timestamp = Timestamp{1'000 + static_cast<std::int64_t>(sequence_)},
                                              .command = command},
                             events_);
    }

    [[nodiscard]] const ShardEngine& engine() const noexcept { return engine_; }

private:
    ShardEngine engine_;
    EventBuffer events_;
    std::uint64_t sequence_{0};
};

NewOrder limit(Side side, std::int64_t price, std::uint64_t quantity, std::uint64_t client_id) {
    return NewOrder{.trader = trader,
                    .client_order_id = ClientOrderId{client_id},
                    .instrument = instrument_one,
                    .side = side,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = Price{price},
                    .quantity = Quantity{quantity}};
}

constexpr std::size_t resting_target = 1'000;
constexpr std::int64_t mid_price = 10'000;
constexpr std::int64_t price_levels = 100;

/// Steady state around 1 000 resting orders: every iteration rests one new
/// order and cancels the oldest one, spread over 100 price levels per side so
/// the book keeps a realistic number of levels. Both commands are timed.
void bm_rest_and_cancel(benchmark::State& state) {
    Driver driver{1};
    lockstep::bench::LatencySamples samples;
    std::deque<OrderId> resting;
    std::uint64_t client_id = 0;
    std::uint64_t n = 0;

    const auto rest_one = [&] {
        const bool buy = (n % 2) == 0;
        // Buys strictly below mid, sells strictly above: nothing ever crosses.
        const std::int64_t offset = 1 + static_cast<std::int64_t>((n / 2) % price_levels);
        const NewOrder order = limit(buy ? Side::Buy : Side::Sell,
                                     buy ? mid_price - offset : mid_price + offset, 10, ++client_id);
        ++n;
        const auto result = driver.apply(order);
        if (!result) {
            return false;
        }
        resting.push_back(result->order_id);
        return true;
    };

    while (resting.size() < resting_target) {
        if (!rest_one()) {
            state.SkipWithError("warm-up order was rejected");
            return;
        }
    }

    bool ok = true;
    for (auto _ : state) {
        const std::int64_t offset = 1 + static_cast<std::int64_t>((n / 2) % price_levels);
        const bool buy = (n % 2) == 0;
        const Command new_order = limit(buy ? Side::Buy : Side::Sell,
                                        buy ? mid_price - offset : mid_price + offset, 10,
                                        ++client_id);
        ++n;
        CommandResult added = std::unexpected{RejectReason::UnknownOrder};
        samples.measure([&] { added = driver.apply(new_order); });
        const Command cancel =
            CancelOrder{.trader = trader, .instrument = instrument_one, .order_id = resting.front()};
        CommandResult cancelled = std::unexpected{RejectReason::UnknownOrder};
        samples.measure([&] { cancelled = driver.apply(cancel); });
        ok = ok && added.has_value() && cancelled.has_value();
        if (added) {
            resting.push_back(added->order_id);
        }
        resting.pop_front();
        benchmark::DoNotOptimize(added);
    }
    if (!ok) {
        state.SkipWithError("a command was rejected");
        return;
    }
    samples.report(state);
    samples.report_rate(state);
    state.counters["resting"] = static_cast<double>(resting.size());
}

/// A taker sweeping `levels` price levels, one resting order per level. The
/// book is restocked between iterations, outside the timed region, so only the
/// aggressive order is measured (it produces `levels` trades).
void bm_aggressive_fill(benchmark::State& state) {
    const auto levels = state.range(0);
    Driver driver{1};
    lockstep::bench::LatencySamples samples;
    std::uint64_t client_id = 0;
    constexpr std::uint64_t per_level = 10;
    bool ok = true;

    for (auto _ : state) {
        for (std::int64_t level = 0; level < levels; ++level) {
            ok = ok && driver.apply(limit(Side::Sell, mid_price + level, per_level, ++client_id))
                           .has_value();
        }
        const Command taker = limit(Side::Buy, mid_price + levels - 1,
                                    per_level * static_cast<std::uint64_t>(levels), ++client_id);
        CommandResult result = std::unexpected{RejectReason::UnknownOrder};
        samples.measure([&] { result = driver.apply(taker); });
        ok = ok && result.has_value();
        benchmark::DoNotOptimize(result);
    }
    if (!ok) {
        state.SkipWithError("a command was rejected");
        return;
    }
    samples.report(state);
    samples.report_rate(state);
    state.counters["levels"] = static_cast<double>(levels);
}

constexpr std::uint32_t mixed_instruments = 4;
constexpr std::size_t mixed_commands = 50'000;
constexpr std::uint64_t mixed_seed = 20'260'101;

/// The fixed command stream for mixed_flow. A reference engine runs alongside
/// the generator so cancels and modifies can target real order ids; order ids
/// are a deterministic function of the stream (ADR-0004), so a fresh engine
/// replaying the recorded commands reproduces the same ids.
std::vector<Command> mixed_stream() {
    struct Target {
        InstrumentId instrument;
        OrderId order;
    };
    std::mt19937_64 rng{mixed_seed};
    Driver reference{mixed_instruments};
    std::vector<Command> stream;
    std::vector<Target> known;
    stream.reserve(mixed_commands);

    // Raw modulo instead of std::uniform_*_distribution: the distributions are
    // not specified to produce the same sequence across standard libraries.
    const auto below = [&rng](std::uint64_t bound) { return rng() % bound; };
    const auto random_price = [&below] {
        return Price{mid_price - 20 + static_cast<std::int64_t>(below(41))};
    };

    // A target that is still resting on the reference book, or nullopt. Ids of
    // orders that have since filled or been cancelled are dropped on the way,
    // so most cancels and modifies hit a live order, as in real flow.
    const auto live_target = [&]() -> std::optional<Target> {
        while (!known.empty()) {
            const std::size_t slot = below(known.size());
            const Target target = known[slot];
            const OrderBook* book = reference.engine().book(target.instrument);
            if (book != nullptr && book->find(target.order) != nullptr) {
                return target;
            }
            known[slot] = known.back();
            known.pop_back();
        }
        return std::nullopt;
    };

    for (std::size_t i = 0; i < mixed_commands; ++i) {
        const std::uint64_t roll = below(100);
        const InstrumentId instrument{static_cast<std::uint32_t>(1 + below(mixed_instruments))};
        Command command;
        const std::optional<Target> target = roll >= 60 ? live_target() : std::nullopt;
        if (target && roll < 85) {
            command = CancelOrder{
                .trader = trader, .instrument = target->instrument, .order_id = target->order};
        } else if (target) {
            command = ModifyOrder{.trader = trader,
                                  .instrument = target->instrument,
                                  .order_id = target->order,
                                  .new_price = random_price(),
                                  .new_quantity = Quantity{1 + below(50)}};
        } else {
            const bool market = roll >= 55 && roll < 60;
            command = NewOrder{
                .trader = trader,
                .client_order_id = ClientOrderId{i + 1},
                .instrument = instrument,
                .side = below(2) == 0 ? Side::Buy : Side::Sell,
                .type = market ? OrderType::Market : OrderType::Limit,
                .time_in_force = (roll >= 45 && roll < 55) ? TimeInForce::Ioc : TimeInForce::Gtc,
                .price = market ? Price{0} : random_price(),
                .quantity = Quantity{1 + below(50)}};
        }
        const auto result = reference.apply(command);
        if (result && std::holds_alternative<NewOrder>(command)) {
            known.push_back(Target{std::get<NewOrder>(command).instrument, result->order_id});
        }
        stream.push_back(command);
    }
    return stream;
}

/// Replays a fixed seeded stream of limit, IOC, market, cancel and modify
/// commands over four instruments. One benchmark iteration is one pass over
/// the stream on a fresh engine. Rejections (a cancel of an order that
/// already traded, for instance) are part of real flow and are not errors.
void bm_mixed_flow(benchmark::State& state) {
    const std::vector<Command> stream = mixed_stream();
    lockstep::bench::LatencySamples samples{stream.size() * 10};
    std::size_t rejected = 0;

    for (auto _ : state) {
        state.PauseTiming();
        Driver driver{mixed_instruments};
        rejected = 0;
        state.ResumeTiming();
        for (const Command& command : stream) {
            CommandResult result = std::unexpected{RejectReason::UnknownOrder};
            samples.measure([&] { result = driver.apply(command); });
            rejected += result.has_value() ? 0U : 1U;
        }
        benchmark::DoNotOptimize(rejected);
    }
    samples.report(state);
    samples.report_rate(state);
    state.counters["rejected_pct"] =
        100.0 * static_cast<double>(rejected) / static_cast<double>(stream.size());
}

BENCHMARK(bm_rest_and_cancel)->Name("matching/rest_and_cancel")->Iterations(200'000);
BENCHMARK(bm_aggressive_fill)
    ->Name("matching/aggressive_fill")
    ->ArgName("levels")
    ->Arg(1)
    ->Arg(3)
    ->Arg(5)
    ->Iterations(200'000);
BENCHMARK(bm_mixed_flow)->Name("matching/mixed_flow")->Iterations(10);

}  // namespace
