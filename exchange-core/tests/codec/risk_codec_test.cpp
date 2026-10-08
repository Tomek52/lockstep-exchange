#include "lockstep/codec/risk_codec.hpp"

#include <gtest/gtest.h>

namespace lockstep::codec {
namespace {

app::PublishedEvent trade_event(const domain::Trade& trade) {
    return app::PublishedEvent{.shard = domain::ShardId{2},
                               .sequence = domain::SequenceNumber{42},
                               .timestamp = domain::Timestamp{1'700'000'000},
                               .event = trade};
}

domain::Trade sample_trade() {
    return domain::Trade{.instrument = domain::InstrumentId{7},
                         .price = domain::Price{101},
                         .quantity = domain::Quantity{5},
                         .aggressor_side = domain::Side::Buy,
                         .maker_order = domain::OrderId{10},
                         .maker_trader = domain::TraderId{100},
                         .taker_order = domain::OrderId{20},
                         .taker_trader = domain::TraderId{200}};
}

TEST(RiskCodec, TradeYieldsTakerThenMakerReports) {
    const auto trade = sample_trade();
    const auto reports = encode_execution_reports(trade_event(trade), trade);

    const auto& taker = reports[0];
    EXPECT_FALSE(taker.is_maker());
    EXPECT_EQ(taker.trader_id(), 200U);
    EXPECT_EQ(taker.order_id(), 20U);
    EXPECT_EQ(taker.side(), v1::SIDE_BUY) << "taker is on the aggressor side";

    const auto& maker = reports[1];
    EXPECT_TRUE(maker.is_maker());
    EXPECT_EQ(maker.trader_id(), 100U);
    EXPECT_EQ(maker.order_id(), 10U);
    EXPECT_EQ(maker.side(), v1::SIDE_SELL) << "maker is on the opposite side";
}

TEST(RiskCodec, SellAggressorFlipsBothSides) {
    auto trade = sample_trade();
    trade.aggressor_side = domain::Side::Sell;
    const auto reports = encode_execution_reports(trade_event(trade), trade);

    EXPECT_EQ(reports[0].side(), v1::SIDE_SELL);  // taker
    EXPECT_EQ(reports[1].side(), v1::SIDE_BUY);   // maker
}

TEST(RiskCodec, BothReportsCarryProvenanceAndFillDetails) {
    const auto trade = sample_trade();
    const auto reports = encode_execution_reports(trade_event(trade), trade);

    for (const auto& report : reports) {
        EXPECT_EQ(report.shard_id(), 2U);
        EXPECT_EQ(report.shard_sequence(), 42U);
        EXPECT_EQ(report.timestamp_ns(), 1'700'000'000);
        EXPECT_EQ(report.instrument_id(), 7U);
        EXPECT_EQ(report.price_ticks(), 101);
        EXPECT_EQ(report.quantity(), 5U);
    }
}

}  // namespace
}  // namespace lockstep::codec
