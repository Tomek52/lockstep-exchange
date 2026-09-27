#include "lockstep/codec/order_entry_codec.hpp"

#include <set>

#include <gtest/gtest.h>

namespace lockstep::codec {
namespace {

v1::SubmitOrderRequest limit_request() {
    v1::SubmitOrderRequest request;
    request.set_trader_id(11);
    request.set_client_order_id(22);
    request.set_instrument_id(3);
    request.set_side(v1::SIDE_SELL);
    request.set_type(v1::ORDER_TYPE_LIMIT);
    request.set_time_in_force(v1::TIME_IN_FORCE_IOC);
    request.set_price_ticks(-5);  // decoding does not judge values; the domain does
    request.set_quantity(100);
    return request;
}

TEST(OrderEntryCodec, DecodesEveryFieldOfALimitOrder) {
    const auto order = decode(limit_request());
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->trader, domain::TraderId{11});
    EXPECT_EQ(order->client_order_id, domain::ClientOrderId{22});
    EXPECT_EQ(order->instrument, domain::InstrumentId{3});
    EXPECT_EQ(order->side, domain::Side::Sell);
    EXPECT_EQ(order->type, domain::OrderType::Limit);
    EXPECT_EQ(order->time_in_force, domain::TimeInForce::Ioc);
    EXPECT_EQ(order->price, domain::Price{-5});
    EXPECT_EQ(order->quantity, domain::Quantity{100});
}

TEST(OrderEntryCodec, RejectsUnspecifiedAndUnknownEnumValues) {
    auto request = limit_request();
    request.set_side(v1::SIDE_UNSPECIFIED);
    EXPECT_EQ(decode(request).error(), DecodeError::InvalidSide);

    request = limit_request();
    request.set_side(static_cast<v1::Side>(77));  // proto3 enums are open
    EXPECT_EQ(decode(request).error(), DecodeError::InvalidSide);

    request = limit_request();
    request.set_type(v1::ORDER_TYPE_UNSPECIFIED);
    EXPECT_EQ(decode(request).error(), DecodeError::InvalidOrderType);

    request = limit_request();
    request.set_time_in_force(v1::TIME_IN_FORCE_UNSPECIFIED);
    EXPECT_EQ(decode(request).error(), DecodeError::InvalidTimeInForce);
}

TEST(OrderEntryCodec, MarketOrdersAreAlwaysImmediateOrCancel) {
    auto request = limit_request();
    request.set_type(v1::ORDER_TYPE_MARKET);
    request.set_time_in_force(v1::TIME_IN_FORCE_UNSPECIFIED);
    const auto order = decode(request);
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->time_in_force, domain::TimeInForce::Ioc);
}

TEST(OrderEntryCodec, DecodesRiskCommands) {
    v1::RiskCommand command;
    command.set_command_id(5);
    EXPECT_EQ(decode(command).error(), DecodeError::MissingRiskAction);

    command.mutable_kill_switch()->set_engaged(true);
    const auto kill = decode(command);
    ASSERT_TRUE(kill.has_value());
    EXPECT_EQ(std::get<domain::KillSwitch>(*kill),
              (domain::KillSwitch{domain::RiskCommandId{5}, true}));
}

TEST(OrderEntryCodec, EncodesAcceptedAndRejectedReplies) {
    v1::CommandAck accepted;
    encode(app::CommandReply{domain::ShardId{1}, domain::SequenceNumber{9}, domain::Timestamp{77},
                             domain::CommandOutcome{domain::OrderId{123}}},
           accepted);
    EXPECT_EQ(accepted.shard_id(), 1U);
    EXPECT_EQ(accepted.shard_sequence(), 9U);
    EXPECT_EQ(accepted.timestamp_ns(), 77);
    EXPECT_EQ(accepted.accepted().order_id(), 123U);

    v1::CommandAck rejected;
    encode(app::CommandReply{domain::ShardId{0}, domain::SequenceNumber{1}, domain::Timestamp{1},
                             std::unexpected(domain::RejectReason::TradingHalted)},
           rejected);
    EXPECT_EQ(rejected.rejected().reason(), v1::REJECT_REASON_TRADING_HALTED);
    EXPECT_FALSE(rejected.rejected().detail().empty());
}

TEST(OrderEntryCodec, EveryDomainRejectReasonHasADistinctWireValue) {
    using enum domain::RejectReason;
    std::set<v1::RejectReason> seen;
    for (const auto reason :
         {UnknownInstrument, InvalidPrice, InvalidQuantity, TraderBlocked, TradingHalted,
          UnknownOrder, NotOrderOwner, DuplicateClientOrderId, RiskUnavailable}) {
        const auto wire = to_proto(reason);
        EXPECT_NE(wire, v1::REJECT_REASON_UNSPECIFIED);
        EXPECT_TRUE(seen.insert(wire).second)
            << "duplicate mapping for " << domain::to_string(reason);
    }
}

}  // namespace
}  // namespace lockstep::codec
