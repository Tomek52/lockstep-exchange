#include "lockstep/domain/order_book.hpp"

#include <algorithm>
#include <ranges>

namespace lockstep::domain {

namespace {

template <typename Levels>
std::vector<LevelView> aggregate(const Levels& levels) {
    return levels | std::views::transform([](const auto& entry) {
               const auto& [price, level] = entry;
               return LevelView{price, level.total, level.orders.size()};
           }) |
           std::ranges::to<std::vector>();
}

}  // namespace

OrderBook::OrderBook(InstrumentSpec spec) noexcept : spec_{spec} {}

std::optional<Price> OrderBook::best_price(Side side) const noexcept {
    return with_side(side, [](const auto& levels) -> std::optional<Price> {
        if (levels.empty()) {
            return std::nullopt;
        }
        return levels.begin()->first;
    });
}

Quantity OrderBook::quantity_at(Side side, Price price) const noexcept {
    return with_side(side, [price](const auto& levels) {
        const auto it = levels.find(price);
        return it == levels.end() ? Quantity{0} : it->second.total;
    });
}

std::size_t OrderBook::order_count() const noexcept {
    std::size_t count = 0;
    for (const auto side : {Side::Buy, Side::Sell}) {
        with_side(side, [&count](const auto& levels) {
            for (const auto& [price, level] : levels) {
                count += level.orders.size();
            }
        });
    }
    return count;
}

BookSnapshot OrderBook::snapshot() const {
    return BookSnapshot{spec_.id, aggregate(bids_), aggregate(asks_)};
}

const RestingOrder* OrderBook::find(OrderId id) const noexcept {
    // O(orders): acceptable for the skeleton; task 001 adds an id index.
    for (const auto side : {Side::Buy, Side::Sell}) {
        const RestingOrder* found =
            with_side(side, [id](const auto& levels) -> const RestingOrder* {
                for (const auto& [price, level] : levels) {
                    const auto it = std::ranges::find(level.orders, id, &RestingOrder::id);
                    if (it != level.orders.end()) {
                        return &*it;
                    }
                }
                return nullptr;
            });
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

void OrderBook::rest(const RestingOrder& order, EventBuffer& out) {
    with_side(order.side, [&](auto& levels) {
        auto [it, inserted] = levels.try_emplace(order.price);
        Level& level = it->second;
        level.orders.push_back(order);
        level.total += order.remaining;
        out.push(BookLevelChanged{spec_.id, order.side, order.price, level.total});
    });
}

std::expected<Quantity, RejectReason> OrderBook::cancel(OrderId id,
                                                        TraderId requester,
                                                        CancelReason reason,
                                                        EventBuffer& out) {
    const RestingOrder* found = find(id);
    if (found == nullptr) {
        return std::unexpected(RejectReason::UnknownOrder);
    }
    if (found->trader != requester) {
        return std::unexpected(RejectReason::NotOrderOwner);
    }
    const RestingOrder order = *found;  // copy: the erase below invalidates `found`

    with_side(order.side, [&](auto& levels) {
        auto level_it = levels.find(order.price);
        Level& level = level_it->second;
        std::erase_if(level.orders, [id](const RestingOrder& o) { return o.id == id; });
        level.total -= order.remaining;
        const Quantity remaining_at_level = level.total;
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
        out.push(OrderCancelled{order.id, order.trader, spec_.id, order.remaining, reason});
        out.push(BookLevelChanged{spec_.id, order.side, order.price, remaining_at_level});
    });
    return order.remaining;
}

}  // namespace lockstep::domain
