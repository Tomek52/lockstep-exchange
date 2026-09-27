#pragma once

#include <expected>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/reject_reason.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

// Stateless checks of a command against instrument reference data. Stateful
// checks (duplicates, ownership, risk) happen in ShardEngine.

[[nodiscard]] constexpr std::expected<void, RejectReason> validate_quantity(
    Quantity quantity, const InstrumentSpec& spec) noexcept {
    if (quantity == Quantity{0} || quantity > spec.max_order_quantity) {
        return std::unexpected(RejectReason::InvalidQuantity);
    }
    return {};
}

[[nodiscard]] constexpr std::expected<void, RejectReason> validate_limit_price(
    Price price, const InstrumentSpec& spec) noexcept {
    if (price < spec.min_price || price > spec.max_price) {
        return std::unexpected(RejectReason::InvalidPrice);
    }
    return {};
}

[[nodiscard]] constexpr std::expected<void, RejectReason> validate(
    const NewOrder& order, const InstrumentSpec& spec) noexcept {
    return validate_quantity(order.quantity, spec)
        .and_then([&]() -> std::expected<void, RejectReason> {
            if (order.type == OrderType::Market) {
                // Market orders carry no price; a non-zero one signals a client bug.
                if (order.price != Price{0}) {
                    return std::unexpected(RejectReason::InvalidPrice);
                }
                return {};
            }
            return validate_limit_price(order.price, spec);
        });
}

[[nodiscard]] constexpr std::expected<void, RejectReason> validate(
    const ModifyOrder& modify, const InstrumentSpec& spec) noexcept {
    return validate_quantity(modify.new_quantity, spec).and_then([&] {
        return validate_limit_price(modify.new_price, spec);
    });
}

}  // namespace lockstep::domain
