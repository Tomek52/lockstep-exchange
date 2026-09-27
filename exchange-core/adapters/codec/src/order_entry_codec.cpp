#include "lockstep/codec/order_entry_codec.hpp"

#include <string>
#include <utility>

namespace lockstep::codec {

namespace {

using domain::ClientOrderId;
using domain::InstrumentId;
using domain::OrderId;
using domain::Price;
using domain::Quantity;
using domain::TraderId;

std::expected<domain::Side, DecodeError> decode_side(v1::Side side) noexcept {
    switch (side) {
        case v1::SIDE_BUY:
            return domain::Side::Buy;
        case v1::SIDE_SELL:
            return domain::Side::Sell;
        default:
            return std::unexpected(DecodeError::InvalidSide);  // UNSPECIFIED or unknown value
    }
}

std::expected<domain::OrderType, DecodeError> decode_type(v1::OrderType type) noexcept {
    switch (type) {
        case v1::ORDER_TYPE_LIMIT:
            return domain::OrderType::Limit;
        case v1::ORDER_TYPE_MARKET:
            return domain::OrderType::Market;
        default:
            return std::unexpected(DecodeError::InvalidOrderType);
    }
}

std::expected<domain::TimeInForce, DecodeError> decode_tif(v1::TimeInForce tif,
                                                           domain::OrderType type) noexcept {
    if (type == domain::OrderType::Market) {
        return domain::TimeInForce::Ioc;  // market orders never rest, whatever the client sent
    }
    switch (tif) {
        case v1::TIME_IN_FORCE_GTC:
            return domain::TimeInForce::Gtc;
        case v1::TIME_IN_FORCE_IOC:
            return domain::TimeInForce::Ioc;
        default:
            return std::unexpected(DecodeError::InvalidTimeInForce);
    }
}

}  // namespace

std::string_view to_string(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::InvalidSide:
            return "invalid side";
        case DecodeError::InvalidOrderType:
            return "invalid order type";
        case DecodeError::InvalidTimeInForce:
            return "invalid time in force";
        case DecodeError::MissingRiskAction:
            return "risk command without action";
    }
    std::unreachable();
}

std::expected<domain::NewOrder, DecodeError> decode(
    const v1::SubmitOrderRequest& request) noexcept {
    const auto side = decode_side(request.side());
    if (!side) {
        return std::unexpected(side.error());
    }
    const auto type = decode_type(request.type());
    if (!type) {
        return std::unexpected(type.error());
    }
    const auto tif = decode_tif(request.time_in_force(), *type);
    if (!tif) {
        return std::unexpected(tif.error());
    }
    return domain::NewOrder{.trader = TraderId{request.trader_id()},
                            .client_order_id = ClientOrderId{request.client_order_id()},
                            .instrument = InstrumentId{request.instrument_id()},
                            .side = *side,
                            .type = *type,
                            .time_in_force = *tif,
                            .price = Price{request.price_ticks()},
                            .quantity = Quantity{request.quantity()}};
}

std::expected<domain::CancelOrder, DecodeError> decode(
    const v1::CancelOrderRequest& request) noexcept {
    return domain::CancelOrder{TraderId{request.trader_id()}, InstrumentId{request.instrument_id()},
                               OrderId{request.order_id()}};
}

std::expected<domain::ModifyOrder, DecodeError> decode(
    const v1::ModifyOrderRequest& request) noexcept {
    return domain::ModifyOrder{TraderId{request.trader_id()}, InstrumentId{request.instrument_id()},
                               OrderId{request.order_id()}, Price{request.new_price_ticks()},
                               Quantity{request.new_quantity()}};
}

std::expected<domain::Command, DecodeError> decode(const v1::RiskCommand& command) noexcept {
    const domain::RiskCommandId id{command.command_id()};
    switch (command.action_case()) {
        case v1::RiskCommand::kBlockTrader:
            return domain::BlockTrader{id, TraderId{command.block_trader().trader_id()}};
        case v1::RiskCommand::kUnblockTrader:
            return domain::UnblockTrader{id, TraderId{command.unblock_trader().trader_id()}};
        case v1::RiskCommand::kKillSwitch:
            return domain::KillSwitch{id, command.kill_switch().engaged()};
        case v1::RiskCommand::ACTION_NOT_SET:
            return std::unexpected(DecodeError::MissingRiskAction);
    }
    return std::unexpected(DecodeError::MissingRiskAction);  // future oneof members
}

void encode(const app::CommandReply& reply, v1::CommandAck& ack) {
    ack.set_shard_id(reply.shard.value());
    ack.set_shard_sequence(reply.sequence.value());
    ack.set_timestamp_ns(reply.timestamp.value());
    if (reply.result) {
        ack.mutable_accepted()->set_order_id(reply.result->order_id.value());
    } else {
        auto* rejected = ack.mutable_rejected();
        rejected->set_reason(to_proto(reply.result.error()));
        rejected->set_detail(std::string{domain::to_string(reply.result.error())});
    }
}

void encode_rejection(v1::RejectReason reason, std::string_view detail, v1::CommandAck& ack) {
    auto* rejected = ack.mutable_rejected();
    rejected->set_reason(reason);
    rejected->set_detail(std::string{detail});
}

v1::RejectReason to_proto(domain::RejectReason reason) noexcept {
    using enum domain::RejectReason;
    switch (reason) {
        case UnknownInstrument:
            return v1::REJECT_REASON_UNKNOWN_INSTRUMENT;
        case InvalidPrice:
            return v1::REJECT_REASON_INVALID_PRICE;
        case InvalidQuantity:
            return v1::REJECT_REASON_INVALID_QUANTITY;
        case TraderBlocked:
            return v1::REJECT_REASON_TRADER_BLOCKED;
        case TradingHalted:
            return v1::REJECT_REASON_TRADING_HALTED;
        case UnknownOrder:
            return v1::REJECT_REASON_UNKNOWN_ORDER;
        case NotOrderOwner:
            return v1::REJECT_REASON_NOT_ORDER_OWNER;
        case DuplicateClientOrderId:
            return v1::REJECT_REASON_DUPLICATE_CLIENT_ORDER_ID;
        case RiskUnavailable:
            return v1::REJECT_REASON_RISK_UNAVAILABLE;
    }
    std::unreachable();  // exhaustive; -Wswitch catches new enumerators
}

v1::RejectReason to_proto(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::InvalidSide:
            return v1::REJECT_REASON_INVALID_SIDE;
        case DecodeError::InvalidOrderType:
            return v1::REJECT_REASON_INVALID_ORDER_TYPE;
        case DecodeError::InvalidTimeInForce:
            return v1::REJECT_REASON_INVALID_TIME_IN_FORCE;
        case DecodeError::MissingRiskAction:
            return v1::REJECT_REASON_UNSPECIFIED;
    }
    std::unreachable();
}

v1::RejectReason to_proto(app::SubmitError error) noexcept {
    switch (error) {
        case app::SubmitError::Overloaded:
            return v1::REJECT_REASON_OVERLOADED;
        case app::SubmitError::UnknownInstrument:
            return v1::REJECT_REASON_UNKNOWN_INSTRUMENT;
        case app::SubmitError::NotRoutable:
            return v1::REJECT_REASON_UNSPECIFIED;
        case app::SubmitError::ShuttingDown:
            return v1::REJECT_REASON_SHUTTING_DOWN;
    }
    std::unreachable();
}

}  // namespace lockstep::codec
