#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "lockstep/domain/events.hpp"
#include "lockstep/domain/flat_map.hpp"
#include "lockstep/domain/reject_reason.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

/// An order resting on the book.
struct RestingOrder {
    OrderId id;
    TraderId trader;
    ClientOrderId client_order_id;
    Side side{Side::Buy};
    Price price;
    Quantity remaining;

    friend constexpr bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

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
/// SKELETON STATUS: the storage (FIFO per level) is the naive version and
/// matching is not implemented. See docs/tasks/001-order-book-storage.md
/// and docs/tasks/002-matching-limit-market.md.
class OrderBook {
public:
    explicit OrderBook(InstrumentSpec spec) noexcept;

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }

    [[nodiscard]] std::optional<Price> best_price(Side side) const noexcept;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept;
    [[nodiscard]] std::size_t order_count() const noexcept;
    [[nodiscard]] BookSnapshot snapshot() const;
    [[nodiscard]] const RestingOrder* find(OrderId id) const noexcept;

    /// Appends `order` at the tail of its price level and emits BookLevelChanged.
    /// Precondition: the order does not cross the opposite side (the matcher
    /// has already consumed any crossing quantity).
    void rest(const RestingOrder& order, EventBuffer& out);

    /// Removes a resting order. Emits OrderCancelled and BookLevelChanged.
    [[nodiscard]] std::expected<Quantity, RejectReason> cancel(OrderId id,
                                                               TraderId requester,
                                                               CancelReason reason,
                                                               EventBuffer& out);

private:
    struct Level {
        Quantity total;
        std::vector<RestingOrder> orders;  // FIFO; task 001 replaces with a pooled intrusive list
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

    InstrumentSpec spec_;
    BidLevels bids_;
    AskLevels asks_;
};

}  // namespace lockstep::domain
