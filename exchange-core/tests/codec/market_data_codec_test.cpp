#include "lockstep/codec/market_data_codec.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace lockstep::codec {
namespace {

app::PublishedEvent envelope(const domain::Event& event) {
    return app::PublishedEvent{.shard = domain::ShardId{2},
                               .sequence = domain::SequenceNumber{42},
                               .timestamp = domain::Timestamp{1'700'000'000},
                               .event = event};
}

domain::Trade sample_trade() {
    return domain::Trade{.instrument = domain::InstrumentId{7},
                         .price = domain::Price{-101},
                         .quantity = domain::Quantity{5},
                         .aggressor_side = domain::Side::Sell,
                         .maker_order = domain::OrderId{10},
                         .maker_trader = domain::TraderId{100},
                         .taker_order = domain::OrderId{20},
                         .taker_trader = domain::TraderId{200}};
}

void expect_provenance(const v1::SubscribeResponse& out) {
    EXPECT_EQ(out.shard_id(), 2U);
    EXPECT_EQ(out.shard_sequence(), 42U);
    EXPECT_EQ(out.timestamp_ns(), 1'700'000'000);
}

TEST(MarketDataCodec, TradeMapsFieldByField) {
    v1::SubscribeResponse out;
    ASSERT_TRUE(encode(envelope(sample_trade()), out));

    expect_provenance(out);
    ASSERT_EQ(out.event_case(), v1::SubscribeResponse::kTrade);
    EXPECT_EQ(out.trade().instrument_id(), 7U);
    EXPECT_EQ(out.trade().price_ticks(), -101);
    EXPECT_EQ(out.trade().quantity(), 5U);
    EXPECT_EQ(out.trade().aggressor_side(), v1::SIDE_SELL);
}

TEST(MarketDataCodec, TradeDoesNotDiscloseTraderOrOrderIds) {
    v1::SubscribeResponse out;
    ASSERT_TRUE(encode(envelope(sample_trade()), out));

    // The public Trade message has exactly four fields. Building the same
    // message by hand and comparing the wire bytes proves nothing else (no
    // maker/taker trader or order id) was serialised.
    v1::SubscribeResponse expected;
    expected.set_shard_id(2);
    expected.set_shard_sequence(42);
    expected.set_timestamp_ns(1'700'000'000);
    auto& trade = *expected.mutable_trade();
    trade.set_instrument_id(7);
    trade.set_price_ticks(-101);
    trade.set_quantity(5);
    trade.set_aggressor_side(v1::SIDE_SELL);
    EXPECT_EQ(out.SerializeAsString(), expected.SerializeAsString());
}

TEST(MarketDataCodec, BookLevelChangeMapsFieldByField) {
    v1::SubscribeResponse out;
    const domain::BookLevelChanged change{.instrument = domain::InstrumentId{3},
                                          .side = domain::Side::Buy,
                                          .price = domain::Price{250},
                                          .quantity = domain::Quantity{0}};
    ASSERT_TRUE(encode(envelope(change), out));

    expect_provenance(out);
    ASSERT_EQ(out.event_case(), v1::SubscribeResponse::kBookLevelUpdate);
    EXPECT_EQ(out.book_level_update().instrument_id(), 3U);
    EXPECT_EQ(out.book_level_update().side(), v1::SIDE_BUY);
    EXPECT_EQ(out.book_level_update().price_ticks(), 250);
    EXPECT_EQ(out.book_level_update().quantity(), 0U) << "zero removes the level";
}

TEST(MarketDataCodec, InstrumentStatusMapsFieldByField) {
    v1::SubscribeResponse out;
    ASSERT_TRUE(
        encode(envelope(domain::InstrumentStatusChanged{domain::InstrumentId{9}, true}), out));

    expect_provenance(out);
    ASSERT_EQ(out.event_case(), v1::SubscribeResponse::kInstrumentStatus);
    EXPECT_EQ(out.instrument_status().instrument_id(), 9U);
    EXPECT_TRUE(out.instrument_status().halted());
}

TEST(MarketDataCodec, PrivateEventsAreNotPublicAndLeaveNothingBehind) {
    const domain::TraderId trader{55};
    const domain::OrderId order{1};
    const domain::InstrumentId instrument{3};
    const std::vector<domain::Event> private_events{
        domain::OrderAccepted{.order_id = order,
                              .trader = trader,
                              .client_order_id = domain::ClientOrderId{2},
                              .instrument = instrument,
                              .side = domain::Side::Buy,
                              .type = domain::OrderType::Limit,
                              .price = domain::Price{100},
                              .quantity = domain::Quantity{1}},
        domain::OrderCancelled{.order_id = order,
                               .trader = trader,
                               .instrument = instrument,
                               .cancelled_quantity = domain::Quantity{1},
                               .reason = domain::CancelReason::UserRequested},
        domain::OrderModified{.order_id = order,
                              .trader = trader,
                              .instrument = instrument,
                              .price = domain::Price{101},
                              .quantity = domain::Quantity{2},
                              .kept_priority = false},
        domain::RiskCommandApplied{domain::RiskCommandId{4}},
    };
    for (const domain::Event& event : private_events) {
        // Start from a populated message to show a reused buffer is cleared.
        v1::SubscribeResponse out;
        ASSERT_TRUE(encode(envelope(sample_trade()), out));
        EXPECT_FALSE(encode(envelope(event), out));
        EXPECT_EQ(out.ByteSizeLong(), 0U);
    }
}

TEST(MarketDataCodec, ReusedBufferDoesNotKeepThePreviousEventKind) {
    v1::SubscribeResponse out;
    ASSERT_TRUE(encode(envelope(sample_trade()), out));
    ASSERT_TRUE(
        encode(envelope(domain::InstrumentStatusChanged{domain::InstrumentId{1}, false}), out));
    EXPECT_EQ(out.event_case(), v1::SubscribeResponse::kInstrumentStatus);
}

}  // namespace
}  // namespace lockstep::codec
