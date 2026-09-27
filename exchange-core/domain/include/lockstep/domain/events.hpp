#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

enum class EventKind : std::uint8_t {
    OrderAccepted,
    OrderCancelled,
    OrderModified,
    Trade,
    BookLevelChanged,
    InstrumentStatusChanged,
    RiskCommandApplied,
};

enum class CancelReason : std::uint8_t {
    UserRequested,
    ImmediateOrCancel,  ///< IOC or market remainder after matching.
    TraderBlocked,
    KillSwitch,
};

struct OrderAccepted {
    static constexpr EventKind kind = EventKind::OrderAccepted;
    OrderId order_id;
    TraderId trader;
    ClientOrderId client_order_id;
    InstrumentId instrument;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price;
    Quantity quantity;
    friend constexpr bool operator==(const OrderAccepted&, const OrderAccepted&) = default;
};

struct OrderCancelled {
    static constexpr EventKind kind = EventKind::OrderCancelled;
    OrderId order_id;
    TraderId trader;
    InstrumentId instrument;
    Quantity cancelled_quantity;
    CancelReason reason{CancelReason::UserRequested};
    friend constexpr bool operator==(const OrderCancelled&, const OrderCancelled&) = default;
};

struct OrderModified {
    static constexpr EventKind kind = EventKind::OrderModified;
    OrderId order_id;
    TraderId trader;
    InstrumentId instrument;
    Price price;
    Quantity quantity;
    bool kept_priority{false};
    friend constexpr bool operator==(const OrderModified&, const OrderModified&) = default;
};

/// One match between a resting (maker) order and an incoming (taker) order.
struct Trade {
    static constexpr EventKind kind = EventKind::Trade;
    InstrumentId instrument;
    Price price;
    Quantity quantity;
    Side aggressor_side{Side::Buy};
    OrderId maker_order;
    TraderId maker_trader;
    OrderId taker_order;
    TraderId taker_trader;
    friend constexpr bool operator==(const Trade&, const Trade&) = default;
};

/// New aggregate quantity at a price level; zero means the level is gone.
struct BookLevelChanged {
    static constexpr EventKind kind = EventKind::BookLevelChanged;
    InstrumentId instrument;
    Side side{Side::Buy};
    Price price;
    Quantity quantity;
    friend constexpr bool operator==(const BookLevelChanged&, const BookLevelChanged&) = default;
};

struct InstrumentStatusChanged {
    static constexpr EventKind kind = EventKind::InstrumentStatusChanged;
    InstrumentId instrument;
    bool halted{false};
    friend constexpr bool operator==(const InstrumentStatusChanged&,
                                     const InstrumentStatusChanged&) = default;
};

/// Emitted once per shard for each applied risk command; the risk client
/// aggregates these across shards into one CommandApplied ack (ADR-0013).
struct RiskCommandApplied {
    static constexpr EventKind kind = EventKind::RiskCommandApplied;
    RiskCommandId command_id;
    friend constexpr bool operator==(const RiskCommandApplied&,
                                     const RiskCommandApplied&) = default;
};

using Event = std::variant<OrderAccepted,
                           OrderCancelled,
                           OrderModified,
                           Trade,
                           BookLevelChanged,
                           InstrumentStatusChanged,
                           RiskCommandApplied>;

namespace detail {
template <typename T, typename Variant>
inline constexpr bool is_alternative_of = false;

template <typename T, typename... Ts>
inline constexpr bool is_alternative_of<T, std::variant<Ts...>> = (std::same_as<T, Ts> || ...);
}  // namespace detail

/// A domain event: an alternative of Event, tagged with its kind, and trivially
/// copyable so it can travel through lock-free queues by plain copy.
template <typename E>
concept DomainEvent =
    detail::is_alternative_of<E, Event> && std::is_trivially_copyable_v<E> && requires {
        { E::kind } -> std::convertible_to<EventKind>;
    };

static_assert(std::is_trivially_copyable_v<Event>, "events must stay trivially copyable");

/// Output buffer the engine appends to while applying one command.
///
/// The shard runtime owns one instance, clears it per command and reuses its
/// capacity, so steady-state operation does not allocate.
class EventBuffer {
public:
    explicit EventBuffer(std::size_t reserve = 64) { events_.reserve(reserve); }

    template <DomainEvent E>
    void push(const E& event) {
        events_.emplace_back(event);
    }

    [[nodiscard]] std::span<const Event> events() const noexcept { return events_; }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    void clear() noexcept { events_.clear(); }

private:
    std::vector<Event> events_;
};

}  // namespace lockstep::domain
