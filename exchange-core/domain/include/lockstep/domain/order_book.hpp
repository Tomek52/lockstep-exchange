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
/// Pointers returned by find() and front() are invalidated by the next call
/// that mutates the book (rest, reduce_front, cancel, cancel_if).
///
/// Crossing an incoming order against this book (deciding what trades and at
/// what price) is not this class's job: it lives in lockstep::domain::match
/// (matching.hpp), which drives front()/reduce_front() below. This class only
/// stores and mutates the resting side.
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

    /// Order slots ever allocated, live or free. Does not grow while the number
    /// of resting orders stays at or below an earlier peak. For tests and
    /// benchmarks only.
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
    /// cancellations. Returns the number cancelled (0 emits nothing).
    /// `predicate` must not access this book.
    // A template rather than std::function_ref, which is C++26 and not in
    // libstdc++ 14 (ADR-0009).
    template <std::predicate<const RestingOrder&> Pred>
    std::size_t cancel_if(Pred predicate, CancelReason reason, EventBuffer& out);

    /// Removes a resting order and returns its remaining quantity. Emits
    /// OrderCancelled and BookLevelChanged. Fails with UnknownOrder if `id` is
    /// not resting, or NotOrderOwner if `requester` does not own it; on
    /// failure the book is unchanged and nothing is emitted.
    [[nodiscard]] std::expected<Quantity, RejectReason> cancel(OrderId id,
                                                               TraderId requester,
                                                               CancelReason reason,
                                                               EventBuffer& out);

private:
    using Index = OrderPool::Index;

    // Each level is a FIFO threaded through pool_: head is the oldest order,
    // which price-time priority matches first.
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

    // Leaves an empty level in the map: callers iterating the levels must
    // erase it themselves, or their iterator would be invalidated here.
    void remove(Level& level, Index slot) noexcept;

    template <typename Pred>
    std::size_t cancel_matching_at_level(Side side,
                                         Price price,
                                         Level& level,
                                         Pred& predicate,
                                         CancelReason reason,
                                         EventBuffer& out);

    InstrumentSpec spec_;
    BidLevels bids_;
    AskLevels asks_;
    OrderPool pool_;
    // Makes find() and cancel() O(1). Hash order is unspecified, so this map
    // is only ever looked up, never iterated (ADR-0004).
    std::unordered_map<OrderId, Index, StrongIntHash> slot_by_id_;
};

template <std::predicate<const RestingOrder&> Pred>
std::size_t OrderBook::cancel_if(Pred predicate, CancelReason reason, EventBuffer& out) {
    std::size_t cancelled = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
        with_side(side, [&](auto& levels) {
            // flat_map iterates best-to-worst, which makes the event order
            // deterministic (ADR-0004).
            for (auto it = levels.begin(); it != levels.end();) {
                auto&& [price, level] = *it;
                cancelled += cancel_matching_at_level(side, price, level, predicate, reason, out);
                it = level.count == 0 ? levels.erase(it) : std::next(it);
            }
        });
    }
    return cancelled;
}

template <typename Pred>
std::size_t OrderBook::cancel_matching_at_level(
    Side side, Price price, Level& level, Pred& predicate, CancelReason reason, EventBuffer& out) {
    std::size_t cancelled = 0;
    for (Index slot = level.head; slot != OrderPool::npos;) {
        // Read the successor first: remove() puts the slot on the free list.
        const Index next = pool_.node(slot).next;
        const RestingOrder& order = pool_.node(slot).order;
        if (std::invoke(predicate, order)) {
            out.push(OrderCancelled{order.id, order.trader, spec_.id, order.remaining, reason});
            remove(level, slot);
            ++cancelled;
        }
        slot = next;
    }
    if (cancelled > 0) {
        out.push(BookLevelChanged{spec_.id, side, price, level.total});
    }
    return cancelled;
}

}  // namespace lockstep::domain
