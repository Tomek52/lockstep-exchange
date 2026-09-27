#pragma once

#include <cstdint>

#include "lockstep/domain/strong_int.hpp"

namespace lockstep::domain {

/// Limit price in instrument ticks (ADR-0005). Signed so that spreads and
/// price differences are representable; valid order prices are always > 0.
struct Price final : StrongInt<struct PriceTag, std::int64_t>, Additive {
    using StrongInt::StrongInt;
};

/// Order quantity in integer lots.
struct Quantity final : StrongInt<struct QuantityTag, std::uint64_t>, Additive {
    using StrongInt::StrongInt;
};

/// Exchange-assigned order id. Unique across shards without coordination: the
/// top bits carry the shard id (see ShardEngine::next_order_id).
struct OrderId final : StrongInt<struct OrderIdTag, std::uint64_t> {
    using StrongInt::StrongInt;
};

/// Client-assigned order id, unique per trader.
struct ClientOrderId final : StrongInt<struct ClientOrderIdTag, std::uint64_t> {
    using StrongInt::StrongInt;
};

struct TraderId final : StrongInt<struct TraderIdTag, std::uint64_t> {
    using StrongInt::StrongInt;
};

struct InstrumentId final : StrongInt<struct InstrumentIdTag, std::uint32_t> {
    using StrongInt::StrongInt;
};

struct ShardId final : StrongInt<struct ShardIdTag, std::uint32_t> {
    using StrongInt::StrongInt;
};

/// Position of a command in its shard's journal. Starts at 1; 0 means "none".
struct SequenceNumber final : StrongInt<struct SequenceNumberTag, std::uint64_t> {
    using StrongInt::StrongInt;
};

/// Nanoseconds since the Unix epoch. Always an INPUT to the domain (stamped by
/// the shard runtime and journaled with the command); the domain never reads a
/// clock, which is what makes replay deterministic (ADR-0004).
struct Timestamp final : StrongInt<struct TimestampTag, std::int64_t> {
    using StrongInt::StrongInt;
};

/// Idempotency key of a risk command issued by the risk sentinel.
struct RiskCommandId final : StrongInt<struct RiskCommandIdTag, std::uint64_t> {
    using StrongInt::StrongInt;
};

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t { Limit, Market };

enum class TimeInForce : std::uint8_t { Gtc, Ioc };

/// Static reference data for one instrument. Loaded at startup and recorded in
/// the journal header, so replay validates against the same limits.
struct InstrumentSpec {
    InstrumentId id;
    Price min_price{1};
    Price max_price{1'000'000'000};
    Quantity max_order_quantity{1'000'000};

    friend constexpr bool operator==(const InstrumentSpec&, const InstrumentSpec&) = default;
};

}  // namespace lockstep::domain
