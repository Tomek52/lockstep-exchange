#include "lockstep/domain/order_book.hpp"

#include <cassert>
#include <ranges>

namespace lockstep::domain {

namespace {

template <typename Levels>
std::vector<LevelView> aggregate(const Levels& levels) {
    return levels | std::views::transform([](const auto& entry) {
               const auto& [price, level] = entry;
               return LevelView{price, level.total, level.count};
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
    return slot_by_id_.size();
}

BookSnapshot OrderBook::snapshot() const {
    return BookSnapshot{spec_.id, aggregate(bids_), aggregate(asks_)};
}

const RestingOrder* OrderBook::find(OrderId id) const noexcept {
    const auto it = slot_by_id_.find(id);
    return it == slot_by_id_.end() ? nullptr : &pool_.node(it->second).order;
}

bool OrderBook::has_resting_client_order(TraderId trader,
                                         ClientOrderId client_order_id) const noexcept {
    return client_order_ids_.contains(ClientOrderKey{trader, client_order_id});
}

const RestingOrder* OrderBook::front(Side side) const noexcept {
    return with_side(side, [this](const auto& levels) -> const RestingOrder* {
        if (levels.empty()) {
            return nullptr;
        }
        return &pool_.node(levels.begin()->second.head).order;
    });
}

void OrderBook::rest(const RestingOrder& order, EventBuffer& out) {
    const Index slot = pool_.acquire(order);
    [[maybe_unused]] const auto [pos, inserted] = slot_by_id_.try_emplace(order.id, slot);
    assert(inserted && "order id is already resting");
    [[maybe_unused]] const auto [cid_pos, cid_inserted] =
        client_order_ids_.insert(ClientOrderKey{order.trader, order.client_order_id});
    // ShardEngine rejects a NewOrder whose (trader, client_order_id) is still
    // resting (RejectReason::DuplicateClientOrderId), and ModifyOrder's
    // cancel/replace always take()s the old entry - which erases this key -
    // before resting the replacement, so this insert never collides.
    assert(cid_inserted && "client order id is already resting for this trader");

    with_side(order.side, [&](auto& levels) {
        auto&& [price, level] = *levels.try_emplace(order.price).first;
        pool_.node(slot).prev = level.tail;
        if (level.tail == OrderPool::npos) {
            level.head = slot;
        } else {
            pool_.node(level.tail).next = slot;
        }
        level.tail = slot;
        ++level.count;
        level.total += order.remaining;
        out.push(BookLevelChanged{spec_.id, order.side, price, level.total});
    });
}

void OrderBook::reduce_front(Side side, Quantity quantity, EventBuffer& out) {
    with_side(side, [&](auto& levels) {
        assert(!levels.empty() && "reduce_front on an empty side");
        const auto it = levels.begin();
        auto&& [price, level] = *it;
        RestingOrder& order = pool_.node(level.head).order;
        assert(Quantity{0} < quantity && quantity <= order.remaining);

        order.remaining -= quantity;
        level.total -= quantity;
        if (order.remaining == Quantity{0}) {
            remove(level, level.head);
        }
        out.push(BookLevelChanged{spec_.id, side, price, level.total});
        if (level.count == 0) {
            levels.erase(it);
        }
    });
}

void OrderBook::reduce(OrderId id, Quantity quantity, EventBuffer& out) {
    const auto found = slot_by_id_.find(id);
    assert(found != slot_by_id_.end() && "reduce of an order that is not resting");
    RestingOrder& order = pool_.node(found->second).order;
    assert(Quantity{0} < quantity && quantity < order.remaining);

    const Quantity delta = order.remaining - quantity;
    order.remaining = quantity;
    with_side(order.side, [&](auto& levels) {
        auto&& [price, level] = *levels.find(order.price);
        level.total -= delta;
        out.push(BookLevelChanged{spec_.id, order.side, price, level.total});
    });
}

std::optional<RestingOrder> OrderBook::take(OrderId id, EventBuffer& out) {
    const auto found = slot_by_id_.find(id);
    if (found == slot_by_id_.end()) {
        return std::nullopt;
    }
    const Index slot = found->second;
    const RestingOrder order = pool_.node(slot).order;  // copy: remove() frees the slot

    with_side(order.side, [&](auto& levels) {
        const auto level_it = levels.find(order.price);
        Level& level = level_it->second;
        remove(level, slot);
        const Quantity remaining_at_level = level.total;
        if (level.count == 0) {
            levels.erase(level_it);
        }
        out.push(BookLevelChanged{spec_.id, order.side, order.price, remaining_at_level});
    });
    return order;
}

std::expected<Quantity, RejectReason> OrderBook::cancel(OrderId id,
                                                        TraderId requester,
                                                        CancelReason reason,
                                                        EventBuffer& out) {
    const auto found = slot_by_id_.find(id);
    if (found == slot_by_id_.end()) {
        return std::unexpected(RejectReason::UnknownOrder);
    }
    const Index slot = found->second;
    const RestingOrder order = pool_.node(slot).order;  // copy: remove() frees the slot
    if (order.trader != requester) {
        return std::unexpected(RejectReason::NotOrderOwner);
    }

    with_side(order.side, [&](auto& levels) {
        const auto level_it = levels.find(order.price);
        Level& level = level_it->second;
        remove(level, slot);
        const Quantity remaining_at_level = level.total;
        if (level.count == 0) {
            levels.erase(level_it);
        }
        out.push(OrderCancelled{order.id, order.trader, spec_.id, order.remaining, reason});
        out.push(BookLevelChanged{spec_.id, order.side, order.price, remaining_at_level});
    });
    return order.remaining;
}

void OrderBook::remove(Level& level, Index slot) noexcept {
    const OrderPool::Node& node = pool_.node(slot);
    if (node.prev == OrderPool::npos) {
        level.head = node.next;
    } else {
        pool_.node(node.prev).next = node.next;
    }
    if (node.next == OrderPool::npos) {
        level.tail = node.prev;
    } else {
        pool_.node(node.next).prev = node.prev;
    }
    --level.count;
    level.total -= node.order.remaining;
    slot_by_id_.erase(node.order.id);
    client_order_ids_.erase(ClientOrderKey{node.order.trader, node.order.client_order_id});
    pool_.release(slot);
}

}  // namespace lockstep::domain
