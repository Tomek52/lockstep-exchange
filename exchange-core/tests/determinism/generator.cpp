#include "generator.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

namespace lockstep::test {

void AcceptedOrders::record(domain::OrderId id) {
    std::lock_guard lock{mutex_};
    // Bounded so a long-running producer does not grow this unboundedly;
    // the oldest ids are the ones most likely to already be filled/cancelled
    // anyway, so dropping them costs little realism.
    constexpr std::size_t cap = 128;
    if (ids_.size() >= cap) {
        ids_.erase(ids_.begin());
    }
    ids_.push_back(id);
}

std::optional<domain::OrderId> AcceptedOrders::sample(std::mt19937_64& rng) const {
    std::lock_guard lock{mutex_};
    if (ids_.empty()) {
        return std::nullopt;
    }
    return ids_[rng() % ids_.size()];
}

domain::Command random_command(std::mt19937_64& rng,
                               domain::TraderId trader,
                               AcceptedOrders& accepted) {
    using namespace domain;

    std::uniform_int_distribution<std::uint32_t> instrument_dist{1, instrument_count};
    const InstrumentId instrument{instrument_dist(rng)};
    std::uniform_int_distribution<int> kind{0, 99};
    const int roll = kind(rng);

    // Cancels and modifies: ~15% each, preferring a real id when one is
    // known so some of them actually succeed (and exercise the book's
    // cancel/modify paths), an unknown one otherwise (RejectReason::
    // UnknownOrder) - both outcomes are rejections the journal must still
    // record and replay identically.
    if (roll < 15) {
        const OrderId target = accepted.sample(rng).value_or(OrderId{rng() % 64});
        return CancelOrder{.trader = trader, .instrument = instrument, .order_id = target};
    }
    if (roll < 30) {
        const OrderId target = accepted.sample(rng).value_or(OrderId{rng() % 64});
        std::uniform_int_distribution<std::int64_t> price{1, 120};
        std::uniform_int_distribution<std::uint64_t> qty{1, 50};
        return ModifyOrder{.trader = trader,
                           .instrument = instrument,
                           .order_id = target,
                           .new_price = Price{price(rng)},
                           .new_quantity = Quantity{qty(rng)}};
    }

    // Invalid price/quantity (0 is outside every instrument's valid range,
    // InstrumentSpec's defaults) and a narrow client-order-id range (frequent
    // collisions, so DuplicateClientOrderId fires often too): every one of
    // these is a rejection the journal must still record and replay
    // identically, which is exactly why they are worth generating.
    const bool invalid_price = roll < 34;  // ~4%: domain rejects (journaled)
    const bool invalid_qty = roll < 38;    // ~4%: domain rejects (journaled)
    std::uniform_int_distribution<std::int64_t> price_dist{1, 120};
    std::uniform_int_distribution<std::uint64_t> qty_dist{1, 50};
    std::uniform_int_distribution<std::uint64_t> client_id_dist{1, 40};  // narrow: frequent dupes

    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::Gtc;
    if (roll < 48) {  // ~10%: market order
        type = OrderType::Market;
    } else if (roll < 58) {  // ~10%: IOC limit order
        tif = TimeInForce::Ioc;
    }

    const Price price =
        (type == OrderType::Market || invalid_price) ? Price{0} : Price{price_dist(rng)};
    const Quantity quantity = invalid_qty ? Quantity{0} : Quantity{qty_dist(rng)};
    return NewOrder{.trader = trader,
                    .client_order_id = ClientOrderId{client_id_dist(rng)},
                    .instrument = instrument,
                    .side = (rng() % 2 == 0) ? Side::Buy : Side::Sell,
                    .type = type,
                    .time_in_force = tif,
                    .price = price,
                    .quantity = quantity};
}

namespace {
// Shared across every producer thread: RiskCommandId is an idempotency key
// (ADR-0013), so every broadcast in the whole run needs a distinct one.
std::atomic<std::uint64_t> next_risk_command_id{1};
}  // namespace

std::optional<domain::Command> maybe_risk_command(std::mt19937_64& rng,
                                                  domain::TraderId target_trader) {
    std::uniform_int_distribution<int> roll{0, risk_command_every - 1};
    if (roll(rng) != 0) {
        return std::nullopt;
    }
    const domain::RiskCommandId id{next_risk_command_id.fetch_add(1, std::memory_order_relaxed)};
    std::uniform_int_distribution<int> which{0, 2};
    switch (which(rng)) {
        case 0:
            return domain::BlockTrader{.command_id = id, .trader = target_trader};
        case 1:
            return domain::UnblockTrader{.command_id = id, .trader = target_trader};
        default:
            return domain::KillSwitch{.command_id = id, .engaged = rng() % 2 == 0};
    }
}

void run_workload(app::Engine& engine,
                  RecordingSubscriber& subscriber,
                  std::vector<app::CommandReply>& live_replies,
                  std::uint64_t producer_count,
                  int commands_per_producer) {
    using namespace domain;

    engine.add_subscriber(subscriber);
    engine.start();

    // Several producers race: the interleaving differs from run to run,
    // which is exactly what the journal must capture. Each has its own
    // trader id and its own view of the orders it has seen accepted - kept
    // here, not inside the producer lambda: a completion can still be in
    // flight on the publisher thread after producers.clear() joins the
    // producer threads below (joining only proves they stopped submitting,
    // not that every reply was delivered yet), so anything a completion
    // touches must outlive that join.
    std::vector<AcceptedOrders> accepted_per_producer{producer_count};
    std::vector<std::jthread> producers;
    for (std::uint64_t seed = 1; seed <= producer_count; ++seed) {
        producers.emplace_back([&engine, &live_replies, &accepted_per_producer, seed,
                                commands_per_producer] {
            std::mt19937_64 rng{seed};
            const TraderId trader{seed};
            AcceptedOrders& accepted = accepted_per_producer[seed - 1];
            // Copyable, so every (re)submission gets a fresh Completion.
            const auto record = [&live_replies,
                                 &accepted](const app::CommandReply& reply) noexcept {
                live_replies.push_back(reply);
                if (reply.result.has_value() && reply.result->order_id.value() != 0) {
                    accepted.record(reply.result->order_id);
                }
            };
            for (int i = 0; i < commands_per_producer; ++i) {
                const Command command =
                    maybe_risk_command(rng, trader).value_or(random_command(rng, trader, accepted));
                // Retry only on back-pressure; any other refusal is a test bug
                // and must fail loudly rather than spin forever.
                for (;;) {
                    const auto submitted = std::holds_alternative<BlockTrader>(command) ||
                                                   std::holds_alternative<UnblockTrader>(command) ||
                                                   std::holds_alternative<KillSwitch>(command)
                                               ? engine.broadcast(command)
                                               : engine.submit(command, record);
                    if (submitted || submitted.error() != app::SubmitError::Overloaded) {
                        EXPECT_TRUE(submitted.has_value()) << app::to_string(submitted.error());
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    producers.clear();  // join producers, then drain the engine
    EXPECT_TRUE(engine.broadcast(KillSwitch{RiskCommandId{1'000'000}, true}).has_value());
    engine.stop();
}

}  // namespace lockstep::test
