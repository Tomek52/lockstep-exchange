#include "generator.hpp"

#include <atomic>
#include <mutex>

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

domain::Command random_command(std::mt19937_64& rng, domain::TraderId trader,
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
    if (roll < 48) {          // ~10%: market order
        type = OrderType::Market;
    } else if (roll < 58) {   // ~10%: IOC limit order
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
    const domain::RiskCommandId id{
        next_risk_command_id.fetch_add(1, std::memory_order_relaxed)};
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

}  // namespace lockstep::test
