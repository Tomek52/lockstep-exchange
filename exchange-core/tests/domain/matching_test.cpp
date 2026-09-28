#include "lockstep/domain/matching.hpp"

#include <random>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/domain/shard_engine.hpp"

namespace lockstep::domain {
namespace {

constexpr InstrumentId instrument{5};

SequencedCommand sequenced(Command command, std::uint64_t seq = 1) {
    return SequencedCommand{SequenceNumber{seq}, Timestamp{1'000 * static_cast<std::int64_t>(seq)},
                            std::move(command)};
}

NewOrder limit_order(Side side,
                     std::int64_t price,
                     std::uint64_t qty,
                     TraderId trader = TraderId{1},
                     TimeInForce tif = TimeInForce::Gtc) {
    return NewOrder{.trader = trader,
                    .client_order_id = ClientOrderId{1},
                    .instrument = instrument,
                    .side = side,
                    .type = OrderType::Limit,
                    .time_in_force = tif,
                    .price = Price{price},
                    .quantity = Quantity{qty}};
}

NewOrder market_order(Side side, std::uint64_t qty, TraderId trader = TraderId{1}) {
    return NewOrder{.trader = trader,
                    .client_order_id = ClientOrderId{1},
                    .instrument = instrument,
                    .side = side,
                    .type = OrderType::Market,
                    .time_in_force = TimeInForce::Ioc,
                    .price = Price{0},
                    .quantity = Quantity{qty}};
}

class MatchingTest : public ::testing::Test {
protected:
    ShardEngine engine{ShardConfig{.shard = ShardId{1}, .instruments = {{.id = instrument}}}};
    EventBuffer out;
};

TEST_F(MatchingTest, NonCrossingLimitRestsWithoutTrade) {
    const auto result = engine.apply(sequenced(limit_order(Side::Buy, 100, 5)), out);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(out.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(out.events()[0]));
    const auto& level = std::get<BookLevelChanged>(out.events()[1]);
    EXPECT_EQ(level.side, Side::Buy);
    EXPECT_EQ(level.price, Price{100});
    EXPECT_EQ(level.quantity, Quantity{5});
    EXPECT_EQ(engine.book(instrument)->best_price(Side::Buy), Price{100});
}

TEST_F(MatchingTest, FullFillAgainstOneRestingOrderEmptiesTheBook) {
    ASSERT_TRUE(
        engine.apply(sequenced(limit_order(Side::Sell, 100, 5, TraderId{1}), 1), out).has_value());
    const OrderId maker_id = std::get<OrderAccepted>(out.events()[0]).order_id;
    out.clear();

    const auto result =
        engine.apply(sequenced(limit_order(Side::Buy, 100, 5, TraderId{2}), 2), out);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(out.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(out.events()[0]));
    const OrderId taker_id = std::get<OrderAccepted>(out.events()[0]).order_id;

    const auto& trade = std::get<Trade>(out.events()[1]);
    EXPECT_EQ(trade.instrument, instrument);
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{5});
    EXPECT_EQ(trade.aggressor_side, Side::Buy);
    EXPECT_EQ(trade.maker_order, maker_id);
    EXPECT_EQ(trade.maker_trader, TraderId{1});
    EXPECT_EQ(trade.taker_order, taker_id);
    EXPECT_EQ(trade.taker_trader, TraderId{2});

    const auto& level = std::get<BookLevelChanged>(out.events()[2]);
    EXPECT_EQ(level.side, Side::Sell);
    EXPECT_EQ(level.price, Price{100});
    EXPECT_EQ(level.quantity, Quantity{0});

    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Sell).has_value());
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
    EXPECT_EQ(engine.book(instrument)->order_count(), 0U);
}

TEST_F(MatchingTest, PartialFillRestsRemainderOnTheTakersSide) {
    ASSERT_TRUE(engine.apply(sequenced(limit_order(Side::Sell, 100, 4), 1), out).has_value());
    out.clear();

    const auto result =
        engine.apply(sequenced(limit_order(Side::Buy, 101, 10, TraderId{2}), 2), out);

    ASSERT_TRUE(result.has_value());
    // OrderAccepted, Trade(4@100), BookLevelChanged(ask 100 -> 0), BookLevelChanged(bid 101 -> 6).
    ASSERT_EQ(out.size(), 4U);
    const auto& trade = std::get<Trade>(out.events()[1]);
    EXPECT_EQ(trade.price, Price{100});
    EXPECT_EQ(trade.quantity, Quantity{4});

    const auto& ask_level = std::get<BookLevelChanged>(out.events()[2]);
    EXPECT_EQ(ask_level.side, Side::Sell);
    EXPECT_EQ(ask_level.quantity, Quantity{0});

    const auto& bid_level = std::get<BookLevelChanged>(out.events()[3]);
    EXPECT_EQ(bid_level.side, Side::Buy);
    EXPECT_EQ(bid_level.price, Price{101});
    EXPECT_EQ(bid_level.quantity, Quantity{6});

    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Sell).has_value());
    EXPECT_EQ(engine.book(instrument)->quantity_at(Side::Buy, Price{101}), Quantity{6});
}

TEST_F(MatchingTest, SweepsMultipleLevelsBestPriceFirst) {
    ASSERT_TRUE(engine.apply(sequenced(limit_order(Side::Sell, 100, 5), 1), out).has_value());
    ASSERT_TRUE(engine.apply(sequenced(limit_order(Side::Sell, 101, 5), 2), out).has_value());
    ASSERT_TRUE(engine.apply(sequenced(limit_order(Side::Sell, 102, 5), 3), out).has_value());
    out.clear();

    const auto result =
        engine.apply(sequenced(limit_order(Side::Buy, 102, 12, TraderId{2}), 4), out);

    ASSERT_TRUE(result.has_value());
    // OrderAccepted, then (Trade, BookLevelChanged) at 100, 101, then 102 (2 lots left).
    ASSERT_EQ(out.size(), 7U);
    const std::vector<std::pair<std::int64_t, std::uint64_t>> expected_trades{
        {100, 5}, {101, 5}, {102, 2}};
    for (std::size_t i = 0; i < expected_trades.size(); ++i) {
        const auto& trade = std::get<Trade>(out.events()[1 + (2 * i)]);
        EXPECT_EQ(trade.price, Price{expected_trades[i].first});
        EXPECT_EQ(trade.quantity, Quantity{expected_trades[i].second});
    }
    const auto& last_level = std::get<BookLevelChanged>(out.events().back());
    EXPECT_EQ(last_level.price, Price{102});
    EXPECT_EQ(last_level.quantity, Quantity{3});  // 5 resting - 2 taken.

    EXPECT_EQ(engine.book(instrument)->best_price(Side::Sell), Price{102});
    EXPECT_EQ(engine.book(instrument)->quantity_at(Side::Sell, Price{102}), Quantity{3});
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
}

TEST_F(MatchingTest, FifoWithinALevelFillsTheOldestOrderFirst) {
    ASSERT_TRUE(
        engine.apply(sequenced(limit_order(Side::Sell, 100, 3, TraderId{1}), 1), out).has_value());
    const OrderId first_id = std::get<OrderAccepted>(out.events()[0]).order_id;
    out.clear();
    ASSERT_TRUE(
        engine.apply(sequenced(limit_order(Side::Sell, 100, 3, TraderId{2}), 2), out).has_value());
    const OrderId second_id = std::get<OrderAccepted>(out.events()[0]).order_id;
    out.clear();

    ASSERT_TRUE(
        engine.apply(sequenced(limit_order(Side::Buy, 100, 1, TraderId{3}), 3), out).has_value());

    ASSERT_EQ(out.size(), 3U);
    const auto& trade = std::get<Trade>(out.events()[1]);
    EXPECT_EQ(trade.maker_order, first_id);
    EXPECT_NE(trade.maker_order, second_id);

    const RestingOrder* first_remaining = engine.book(instrument)->find(first_id);
    ASSERT_NE(first_remaining, nullptr);
    EXPECT_EQ(first_remaining->remaining, Quantity{2});
    const RestingOrder* second_untouched = engine.book(instrument)->find(second_id);
    ASSERT_NE(second_untouched, nullptr);
    EXPECT_EQ(second_untouched->remaining, Quantity{3});
}

TEST_F(MatchingTest, TakerGetsPriceImprovementAtTheMakersPrice) {
    ASSERT_TRUE(engine.apply(sequenced(limit_order(Side::Sell, 100, 5), 1), out).has_value());
    out.clear();

    ASSERT_TRUE(
        engine.apply(sequenced(limit_order(Side::Buy, 105, 5, TraderId{2}), 2), out).has_value());

    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(std::get<Trade>(out.events()[1]).price, Price{100});
}

TEST_F(MatchingTest, MarketOrderOnAnEmptyBookIsFullyCancelled) {
    const auto result = engine.apply(sequenced(market_order(Side::Buy, 7)), out);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(out.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(out.events()[0]));
    const auto& cancelled = std::get<OrderCancelled>(out.events()[1]);
    EXPECT_EQ(cancelled.order_id, result->order_id);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{7});
    EXPECT_EQ(cancelled.reason, CancelReason::ImmediateOrCancel);
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
    EXPECT_EQ(engine.book(instrument)->order_count(), 0U);
}

TEST_F(MatchingTest, IocLimitRemainderIsCancelledNeverRested) {
    ASSERT_TRUE(engine.apply(sequenced(limit_order(Side::Sell, 100, 3), 1), out).has_value());
    out.clear();

    const auto result = engine.apply(
        sequenced(limit_order(Side::Buy, 100, 10, TraderId{2}, TimeInForce::Ioc), 2), out);

    ASSERT_TRUE(result.has_value());
    // OrderAccepted, Trade(3@100), BookLevelChanged(ask -> 0), OrderCancelled(7, IOC).
    ASSERT_EQ(out.size(), 4U);
    const auto& cancelled = std::get<OrderCancelled>(out.events().back());
    EXPECT_EQ(cancelled.order_id, result->order_id);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{7});
    EXPECT_EQ(cancelled.reason, CancelReason::ImmediateOrCancel);
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
}

// Drives 1'000 pseudo-random commands (fixed seed) into a fresh engine and
// returns every event produced, in order.
std::vector<Event> run_random_commands() {
    ShardEngine local_engine{ShardConfig{.shard = ShardId{1}, .instruments = {{.id = instrument}}}};
    std::mt19937_64 rng{20020202};
    std::uniform_int_distribution<int> command_kind{0, 4};  // 0: cancel, else: new order
    std::uniform_int_distribution<std::uint64_t> trader{0, 4};
    std::uniform_int_distribution<std::int64_t> price{90, 110};
    std::uniform_int_distribution<std::uint64_t> qty{0, 10};  // 0: domain rejects (no events)
    std::uniform_int_distribution<std::uint64_t> order_id{0, 199};

    EventBuffer local_out;
    std::vector<Event> all_events;
    for (std::uint64_t seq = 1; seq <= 1'000; ++seq) {
        Command command;
        if (command_kind(rng) == 0) {
            command = CancelOrder{TraderId{trader(rng)}, instrument, OrderId{order_id(rng)}};
        } else {
            const OrderType type = (command_kind(rng) == 1) ? OrderType::Market : OrderType::Limit;
            command =
                NewOrder{.trader = TraderId{trader(rng)},
                         .client_order_id = ClientOrderId{seq},
                         .instrument = instrument,
                         .side = (rng() % 2 == 0) ? Side::Buy : Side::Sell,
                         .type = type,
                         .time_in_force = (rng() % 2 == 0) ? TimeInForce::Gtc : TimeInForce::Ioc,
                         .price = (type == OrderType::Market) ? Price{0} : Price{price(rng)},
                         .quantity = Quantity{qty(rng)}};
        }
        local_out.clear();
        (void)local_engine.apply(sequenced(command, seq), local_out);
        all_events.insert(all_events.end(), local_out.events().begin(), local_out.events().end());
    }
    return all_events;
}

TEST_F(MatchingTest, IdenticalCommandSequencesProduceIdenticalEvents) {
    // ADR-0004: apply() must be a pure function of the command sequence, so two
    // independent engines fed the same 1'000 commands must diverge nowhere.
    const std::vector<Event> first_run = run_random_commands();
    const std::vector<Event> second_run = run_random_commands();

    ASSERT_EQ(first_run.size(), second_run.size());
    for (std::size_t i = 0; i < first_run.size(); ++i) {
        ASSERT_EQ(first_run[i], second_run[i]) << "first divergence at event " << i;
    }
}

}  // namespace
}  // namespace lockstep::domain
