#pragma once

#include <cstdint>
#include <variant>

#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

/// Stable on-disk identity of each command type. The journal stores this tag,
/// never std::variant::index(), so reordering Command's alternatives cannot
/// silently corrupt existing journals (ADR-0012). Values are append-only.
enum class CommandTag : std::uint8_t {
    NewOrder = 1,
    CancelOrder = 2,
    ModifyOrder = 3,
    BlockTrader = 4,
    UnblockTrader = 5,
    KillSwitch = 6,
    RiskLinkStatus = 7,
};

// Commands are plain aggregates: every field is an input that the journal
// persists. Anything that influences the outcome must be a field here, or
// replay would diverge (ADR-0004).

struct NewOrder {
    static constexpr CommandTag tag = CommandTag::NewOrder;
    TraderId trader;
    ClientOrderId client_order_id;
    InstrumentId instrument;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce time_in_force{TimeInForce::Gtc};
    Price price;  ///< Must be 0 for market orders.
    Quantity quantity;
    friend constexpr bool operator==(const NewOrder&, const NewOrder&) = default;
};

struct CancelOrder {
    static constexpr CommandTag tag = CommandTag::CancelOrder;
    TraderId trader;
    InstrumentId instrument;
    OrderId order_id;
    friend constexpr bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

struct ModifyOrder {
    static constexpr CommandTag tag = CommandTag::ModifyOrder;
    TraderId trader;
    InstrumentId instrument;
    OrderId order_id;
    Price new_price;
    Quantity new_quantity;
    friend constexpr bool operator==(const ModifyOrder&, const ModifyOrder&) = default;
};

/// Risk commands are broadcast to every shard; each shard applies them to the
/// traders/instruments it owns.
struct BlockTrader {
    static constexpr CommandTag tag = CommandTag::BlockTrader;
    RiskCommandId command_id;
    TraderId trader;
    friend constexpr bool operator==(const BlockTrader&, const BlockTrader&) = default;
};

struct UnblockTrader {
    static constexpr CommandTag tag = CommandTag::UnblockTrader;
    RiskCommandId command_id;
    TraderId trader;
    friend constexpr bool operator==(const UnblockTrader&, const UnblockTrader&) = default;
};

struct KillSwitch {
    static constexpr CommandTag tag = CommandTag::KillSwitch;
    RiskCommandId command_id;
    bool engaged{true};
    friend constexpr bool operator==(const KillSwitch&, const KillSwitch&) = default;
};

/// Connectivity to the risk sentinel changed. Journaled because, under the
/// fail-closed policy, it changes which orders are accepted (ADR-0013).
struct RiskLinkStatus {
    static constexpr CommandTag tag = CommandTag::RiskLinkStatus;
    bool connected{false};
    friend constexpr bool operator==(const RiskLinkStatus&, const RiskLinkStatus&) = default;
};

using Command = std::variant<NewOrder,
                             CancelOrder,
                             ModifyOrder,
                             BlockTrader,
                             UnblockTrader,
                             KillSwitch,
                             RiskLinkStatus>;

/// A command after the shard runtime assigned its position and time. This is
/// exactly what the journal stores and what replay feeds back in.
struct SequencedCommand {
    SequenceNumber sequence;
    Timestamp timestamp;
    Command command;
    friend constexpr bool operator==(const SequencedCommand&, const SequencedCommand&) = default;
};

}  // namespace lockstep::domain
