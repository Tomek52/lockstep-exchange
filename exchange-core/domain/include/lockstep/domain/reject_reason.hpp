#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

namespace lockstep::domain {

/// Every way the domain can refuse a command. Returned through std::expected;
/// the domain never throws (ADR-0008). Transport-level refusals (overload,
/// shutdown, malformed messages) are not domain concepts and live in adapters.
enum class RejectReason : std::uint8_t {
    UnknownInstrument,
    InvalidPrice,
    InvalidQuantity,
    TraderBlocked,
    TradingHalted,
    UnknownOrder,
    NotOrderOwner,
    DuplicateClientOrderId,
    RiskUnavailable,
};

[[nodiscard]] constexpr std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::UnknownInstrument:
            return "unknown instrument";
        case RejectReason::InvalidPrice:
            return "invalid price";
        case RejectReason::InvalidQuantity:
            return "invalid quantity";
        case RejectReason::TraderBlocked:
            return "trader blocked";
        case RejectReason::TradingHalted:
            return "trading halted";
        case RejectReason::UnknownOrder:
            return "unknown order";
        case RejectReason::NotOrderOwner:
            return "not order owner";
        case RejectReason::DuplicateClientOrderId:
            return "duplicate client order id";
        case RejectReason::RiskUnavailable:
            return "risk sentinel unavailable";
    }
    // Every enumerator is handled above and values only ever come from this
    // enum (decoders validate before constructing one), so this is truly
    // unreachable. -Wswitch flags any enumerator added without a case.
    std::unreachable();
}

}  // namespace lockstep::domain
