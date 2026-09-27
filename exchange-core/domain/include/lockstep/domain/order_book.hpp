#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lockstep/domain/events.hpp"
#include "lockstep/domain/flat_map.hpp"
#include "lockstep/domain/order_pool.hpp"
#include "lockstep/domain/reject_reason.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

/// Aggregated view of one price level.
struct LevelView {
    Price price;
    Quantity quantity;
    std::size_t order_count{0};

    friend constexpr bool operator==(const LevelView&, const LevelView&) = default;
};

/// Aggregated book state. Equality of snapshots is what the determinism test
/// compares after replay (ADR-0004).
struct BookSnapshot {
    InstrumentId instrument;
    std::vector<LevelView> bids;  ///< best (highest) first
    std::vector<LevelView> asks;  ///< best (lowest) first

    friend bool operator==(const BookSnapshot&, const BookSnapshot&) = default;
};

/// Limit order book for one instrument with price-time priority.
///
/// Single-writer: exactly one shard thread ever touches a given book, so the
/// type contains no synchronisation at all (ADR-0003). It is also free of I/O,
/// clocks and randomness (ADR-0002, ADR-0004).
///
/// Storage (task 001): price levels live in flat_maps; each level is a FIFO
/// threaded through an OrderPool as an intrusive doubly-linked list, and an
/// id -> slot index makes find and cancel O(1). The index is a hash map used
/// only for lookup and never iterated, so it cannot influence output order
/// (ADR-0004).
///
/// Pointers returned by find() and front() stay valid until the next call
/// that mutates the book.
///
/// SKELETON STATUS: matching is not implemented yet; see
/// docs/tasks/002-matching-limit-market.md.
class OrderBook {
public:
    explicit OrderBook(InstrumentSpec spec) noexcept;

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }

    [[nodiscard]] std::optional<Price> best_price(Side side) const noexcept;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept;
    [[nodiscard]] std::size_t order_count() const noexcept;
    [[nodiscard]] BookSnapshot snapshot() const;
    [[nodiscard]] const RestingOrder* find(OrderId id) const noexcept;

    /// Oldest order at the best price level of `side`, or nullptr if that side is empty.
    [[nodiscard]] const RestingOrder* front(Side side) const noexcept;

    /// Slots ever allocated by the order pool. For tests and benchmarks: it
    /// stops growing once the book has reached its peak depth.
    [[nodiscard]] std::size_t pool_capacity() const noexcept { return pool_.capacity(); }

    /// Appends `order` at the tail of its price level and emits BookLevelChanged.
    /// Preconditions: the order does not cross the opposite side (the matcher
    /// has already consumed any crossing quantity), and its id is not resting.
    void rest(const RestingOrder& order, EventBuffer& out);

    /// Reduces the remaining quantity of the order returned by front(side) by
    /// `quantity` (precondition: 0 < quantity <= front(side)->remaining). Removes
    /// the order when it reaches zero and the level when it becomes empty.
    /// Emits exactly one BookLevelChanged for the affected level.
    void reduce_front(Side side, Quantity quantity, EventBuffer& out);

    /// Cancels every resting order for which `predicate` returns true, in
    /// deterministic order: bids best-to-worst then asks best-to-worst, FIFO
    /// within a level. Emits OrderCancelled for each order and one
    /// BookLevelChanged per touched level, right after that level's
    /// cancellations. Returns the number cancelled.
    ///
    /// A template rather than std::function_ref, which is C++26 and not in
    /// libstdc++ 14 (ADR-0009).
    template <std::predicate<const RestingOrder&> Pred>
    std::size_t cancel_if(Pred predicate, CancelReason reason, EventBuffer& out);

    /// Removes a resting order. Emits OrderCancelled and BookLevelChanged.
    [[nodiscard]] std::expected<Quantity, RejectReason> cancel(OrderId id,
                                                               TraderId requester,
                                                               CancelReason reason,
                                                               EventBuffer& out);

private:
    using Index = OrderPool::Index;

    /// FIFO of pool slots: head is the oldest order (next to match).
    struct Level {
        Quantity total;
        std::uint32_t count{0};
        Index head{OrderPool::npos};
        Index tail{OrderPool::npos};
    };

    using BidLevels = flat_map<Price, Level, std::greater<>>;
    using AskLevels = flat_map<Price, Level, std::less<>>;

    /// Bids and asks have different map types (their comparators differ), so
    /// side-generic code is written once as a generic lambda and dispatched
    /// here. Deducing this forwards the object's constness to the lambda.
    template <typename Self, typename Fn>
    decltype(auto) with_side(this Self& self, Side side, Fn&& fn) {
        if (side == Side::Buy) {
            return std::forward<Fn>(fn)(self.bids_);
        }
        return std::forward<Fn>(fn)(self.asks_);
    }

    /// Unlinks the order in slot `index` from `level`, drops it from the id
    /// index and frees the slot. The caller erases the level if it empties.
    void remove(Level& level, Index index) noexcept;

    InstrumentSpec spec_;
    BidLevels bids_;
    AskLevels asks_;
    OrderPool pool_;
    std::unordered_map<OrderId, Index, StrongIntHash> index_;  // lookup only, never iterated
};

template <std::predicate<const RestingOrder&> Pred>
std::size_t OrderBook::cancel_if(Pred predicate, CancelReason reason, EventBuffer& out) {
    std::size_t cancelled = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
        with_side(side, [&](auto& levels) {
            // flat_map iteration is ordered best-to-worst, which is what makes
            // the event order deterministic (ADR-0004).
            for (auto it = levels.begin(); it != levels.end();) {
                auto&& [price, level] = *it;
                const std::size_t cancelled_before = cancelled;
                for (Index slot = level.head; slot != OrderPool::npos;) {
                    const Index next = pool_.node(slot).next;
                    const RestingOrder& order = pool_.node(slot).order;
                    if (std::invoke(predicate, order)) {
                        out.push(OrderCancelled{order.id, order.trader, spec_.id, order.remaining,
                                                reason});
                        remove(level, slot);
                        ++cancelled;
                    }
                    slot = next;
                }
                if (cancelled != cancelled_before) {
                    out.push(BookLevelChanged{spec_.id, side, price, level.total});
                }
                it = level.count == 0 ? levels.erase(it) : std::next(it);
            }
        });
    }
    return cancelled;
}

}  // namespace lockstep::domain
