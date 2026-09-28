#include "lockstep/domain/matching.hpp"

#include <cstddef>
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

// Every acceptance criterion asserts the complete event sequence, not just a
// prefix of it, so a copy of the buffer is compared against an expected
// vector via each event alternative's defaulted operator==.
std::vector<Event> events_vector(const EventBuffer& out) {
    return {out.events().begin(), out.events().end()};
}

class MatchingTest : public ::testing::Test {
protected:
    ShardEngine engine{ShardConfig{.shard = ShardId{1}, .instruments = {{.id = instrument}}}};
    EventBuffer out;
};

TEST_F(MatchingTest, NonCrossingLimitRestsWithoutTrade) {
    const auto result = engine.apply(sequenced(limit_order(Side::Buy, 100, 5)), out);

    ASSERT_TRUE(result.has_value());
    const OrderId id = result->order_id;
    const std::vector<Event> expected{
        OrderAccepted{id, TraderId{1}, ClientOrderId{1}, instrument, Side::Buy, OrderType::Limit,
                      Price{100}, Quantity{5}},
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{5}},
    };
    EXPECT_EQ(events_vector(out), expected);
    EXPECT_EQ(engine.book(instrument)->best_price(Side::Buy), Price{100});
}

TEST_F(MatchingTest, FullFillAgainstOneRestingOrderEmptiesTheBook) {
    const auto maker =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 5, TraderId{1}), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();

    const auto taker = engine.apply(sequenced(limit_order(Side::Buy, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Buy,
                      OrderType::Limit, Price{100}, Quantity{5}},
        Trade{instrument, Price{100}, Quantity{5}, Side::Buy, maker_id, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
    };
    EXPECT_EQ(events_vector(out), expected);

    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Sell).has_value());
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
    EXPECT_EQ(engine.book(instrument)->order_count(), 0U);
}

TEST_F(MatchingTest, PartialFillRestsRemainderOnTheTakersSide) {
    const auto maker = engine.apply(sequenced(limit_order(Side::Sell, 100, 4), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();

    const auto taker =
        engine.apply(sequenced(limit_order(Side::Buy, 101, 10, TraderId{2}), 2), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Buy,
                      OrderType::Limit, Price{101}, Quantity{10}},
        Trade{instrument, Price{100}, Quantity{4}, Side::Buy, maker_id, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
        BookLevelChanged{instrument, Side::Buy, Price{101}, Quantity{6}},
    };
    EXPECT_EQ(events_vector(out), expected);

    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Sell).has_value());
    EXPECT_EQ(engine.book(instrument)->quantity_at(Side::Buy, Price{101}), Quantity{6});
}

TEST_F(MatchingTest, SweepsMultipleLevelsBestPriceFirst) {
    const auto ask100 = engine.apply(sequenced(limit_order(Side::Sell, 100, 5), 1), out);
    ASSERT_TRUE(ask100.has_value());
    const OrderId id100 = ask100->order_id;
    const auto ask101 = engine.apply(sequenced(limit_order(Side::Sell, 101, 5), 2), out);
    ASSERT_TRUE(ask101.has_value());
    const OrderId id101 = ask101->order_id;
    const auto ask102 = engine.apply(sequenced(limit_order(Side::Sell, 102, 5), 3), out);
    ASSERT_TRUE(ask102.has_value());
    const OrderId id102 = ask102->order_id;
    out.clear();

    const auto taker =
        engine.apply(sequenced(limit_order(Side::Buy, 102, 12, TraderId{2}), 4), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Buy,
                      OrderType::Limit, Price{102}, Quantity{12}},
        Trade{instrument, Price{100}, Quantity{5}, Side::Buy, id100, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
        Trade{instrument, Price{101}, Quantity{5}, Side::Buy, id101, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{101}, Quantity{0}},
        // Best-price-first sweep leaves 2 lots taken from the 5 resting @ 102.
        Trade{instrument, Price{102}, Quantity{2}, Side::Buy, id102, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{102}, Quantity{3}},
    };
    EXPECT_EQ(events_vector(out), expected);

    EXPECT_EQ(engine.book(instrument)->best_price(Side::Sell), Price{102});
    EXPECT_EQ(engine.book(instrument)->quantity_at(Side::Sell, Price{102}), Quantity{3});
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
}

TEST_F(MatchingTest, FifoWithinALevelFillsTheOldestOrderFirst) {
    const auto first =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 3, TraderId{1}), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();
    const auto second =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 3, TraderId{2}), 2), out);
    ASSERT_TRUE(second.has_value());
    const OrderId second_id = second->order_id;
    out.clear();

    const auto taker = engine.apply(sequenced(limit_order(Side::Buy, 100, 1, TraderId{3}), 3), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{3}, ClientOrderId{1}, instrument, Side::Buy,
                      OrderType::Limit, Price{100}, Quantity{1}},
        Trade{instrument, Price{100}, Quantity{1}, Side::Buy, first_id, TraderId{1}, taker_id,
              TraderId{3}},
        // Level total after the fill: first order's 2 remaining + second's untouched 3.
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{5}},
    };
    EXPECT_EQ(events_vector(out), expected);

    const RestingOrder* first_remaining = engine.book(instrument)->find(first_id);
    ASSERT_NE(first_remaining, nullptr);
    EXPECT_EQ(first_remaining->remaining, Quantity{2});
    const RestingOrder* second_untouched = engine.book(instrument)->find(second_id);
    ASSERT_NE(second_untouched, nullptr);
    EXPECT_EQ(second_untouched->remaining, Quantity{3});
}

TEST_F(MatchingTest, TakerGetsPriceImprovementAtTheMakersPrice) {
    const auto maker = engine.apply(sequenced(limit_order(Side::Sell, 100, 5), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();

    const auto taker = engine.apply(sequenced(limit_order(Side::Buy, 105, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Buy,
                      OrderType::Limit, Price{105}, Quantity{5}},
        Trade{instrument, Price{100}, Quantity{5}, Side::Buy, maker_id, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
    };
    EXPECT_EQ(events_vector(out), expected);
}

// The mirror of TakerGetsPriceImprovementAtTheMakersPrice for a sell
// aggressor at an EQUAL price: a mutant that changes the sell branch's `>=`
// to `>` would leave this passing bid unmatched (100 > 100 is false), so it
// pins that operator specifically, not just "crossing happens".
TEST_F(MatchingTest, SellAggressorAtEqualPriceCrossesAndTradesAtTheBid) {
    const auto maker = engine.apply(sequenced(limit_order(Side::Buy, 100, 5), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();

    const auto taker =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Sell,
                      OrderType::Limit, Price{100}, Quantity{5}},
        Trade{instrument, Price{100}, Quantity{5}, Side::Sell, maker_id, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{0}},
    };
    EXPECT_EQ(events_vector(out), expected);
}

TEST_F(MatchingTest, MarketOrderOnAnEmptyBookIsFullyCancelled) {
    const auto result = engine.apply(sequenced(market_order(Side::Buy, 7)), out);

    ASSERT_TRUE(result.has_value());
    const OrderId id = result->order_id;
    const std::vector<Event> expected{
        OrderAccepted{id, TraderId{1}, ClientOrderId{1}, instrument, Side::Buy, OrderType::Market,
                      Price{0}, Quantity{7}},
        OrderCancelled{id, TraderId{1}, instrument, Quantity{7}, CancelReason::ImmediateOrCancel},
    };
    EXPECT_EQ(events_vector(out), expected);
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
    EXPECT_EQ(engine.book(instrument)->order_count(), 0U);
}

// A market order is a limit order with no price cap, so it must sweep every
// resting price on the opposite side, not just the best one, before any
// unfilled remainder is cancelled. A mutant that made market orders match
// like a (price-capped) limit order would stop after the level at price 0
// and leave nothing traded here.
TEST_F(MatchingTest, MarketBuySweepsEveryRestingAskThenCancelsRemainder) {
    const auto ask100 = engine.apply(sequenced(limit_order(Side::Sell, 100, 3), 1), out);
    ASSERT_TRUE(ask100.has_value());
    const OrderId id100 = ask100->order_id;
    const auto ask105 = engine.apply(sequenced(limit_order(Side::Sell, 105, 3), 2), out);
    ASSERT_TRUE(ask105.has_value());
    const OrderId id105 = ask105->order_id;
    out.clear();

    const auto taker = engine.apply(sequenced(market_order(Side::Buy, 10, TraderId{2}), 3), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Buy,
                      OrderType::Market, Price{0}, Quantity{10}},
        Trade{instrument, Price{100}, Quantity{3}, Side::Buy, id100, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
        Trade{instrument, Price{105}, Quantity{3}, Side::Buy, id105, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{105}, Quantity{0}},
        OrderCancelled{taker_id, TraderId{2}, instrument, Quantity{4},
                       CancelReason::ImmediateOrCancel},
    };
    EXPECT_EQ(events_vector(out), expected);
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Sell).has_value());
}

// Mirror of MarketBuySweepsEveryRestingAskThenCancelsRemainder for the sell
// side, against resting bids.
TEST_F(MatchingTest, MarketSellSweepsEveryRestingBidThenCancelsRemainder) {
    const auto bid105 = engine.apply(sequenced(limit_order(Side::Buy, 105, 3), 1), out);
    ASSERT_TRUE(bid105.has_value());
    const OrderId id105 = bid105->order_id;
    const auto bid100 = engine.apply(sequenced(limit_order(Side::Buy, 100, 3), 2), out);
    ASSERT_TRUE(bid100.has_value());
    const OrderId id100 = bid100->order_id;
    out.clear();

    const auto taker = engine.apply(sequenced(market_order(Side::Sell, 10, TraderId{2}), 3), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Sell,
                      OrderType::Market, Price{0}, Quantity{10}},
        // Best bid (105, the higher price) is hit first.
        Trade{instrument, Price{105}, Quantity{3}, Side::Sell, id105, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Buy, Price{105}, Quantity{0}},
        Trade{instrument, Price{100}, Quantity{3}, Side::Sell, id100, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{0}},
        OrderCancelled{taker_id, TraderId{2}, instrument, Quantity{4},
                       CancelReason::ImmediateOrCancel},
    };
    EXPECT_EQ(events_vector(out), expected);
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
}

TEST_F(MatchingTest, IocLimitRemainderIsCancelledNeverRested) {
    const auto maker = engine.apply(sequenced(limit_order(Side::Sell, 100, 3), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();

    const auto taker = engine.apply(
        sequenced(limit_order(Side::Buy, 100, 10, TraderId{2}, TimeInForce::Ioc), 2), out);
    ASSERT_TRUE(taker.has_value());
    const OrderId taker_id = taker->order_id;

    const std::vector<Event> expected{
        OrderAccepted{taker_id, TraderId{2}, ClientOrderId{1}, instrument, Side::Buy,
                      OrderType::Limit, Price{100}, Quantity{10}},
        Trade{instrument, Price{100}, Quantity{3}, Side::Buy, maker_id, TraderId{1}, taker_id,
              TraderId{2}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
        OrderCancelled{taker_id, TraderId{2}, instrument, Quantity{7},
                       CancelReason::ImmediateOrCancel},
    };
    EXPECT_EQ(events_vector(out), expected);
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
}

struct RandomRunResult {
    std::vector<Event> events;
    std::size_t trade_count{0};
    std::size_t successful_cancel_count{0};
};

// Drives 1'000 pseudo-random commands (fixed seed) into a fresh engine and
// returns every event produced, in order, plus how many of them actually
// matched or cancelled something live. CancelOrder targets are drawn from
// ids this same run has already accepted (with their real owning trader),
// rather than from the whole OrderId space: ids are shard<<48|counter
// (ShardEngine::order_id_shard_shift), so a uniformly random uint64 below a
// small bound is essentially never a live order, and a random trader is
// usually not its owner, either of which would make every cancel a no-op
// and leave that branch of the domain unexercised.
RandomRunResult run_random_commands() {
    ShardEngine local_engine{ShardConfig{.shard = ShardId{1}, .instruments = {{.id = instrument}}}};
    std::mt19937_64 rng{20020202};
    std::uniform_int_distribution<int> command_kind{0, 4};  // 0: cancel, else: new order
    std::uniform_int_distribution<std::uint64_t> trader{0, 4};
    std::uniform_int_distribution<std::int64_t> price{90, 110};
    std::uniform_int_distribution<std::uint64_t> qty{0, 10};  // 0: domain rejects (no events)

    struct LiveOrder {
        OrderId id;
        TraderId trader;
    };
    std::vector<LiveOrder> accepted_orders;

    EventBuffer local_out;
    RandomRunResult result;
    for (std::uint64_t seq = 1; seq <= 1'000; ++seq) {
        const bool cancel = command_kind(rng) == 0 && !accepted_orders.empty();
        Command command;
        if (cancel) {
            const LiveOrder& target = accepted_orders[rng() % accepted_orders.size()];
            command = CancelOrder{target.trader, instrument, target.id};
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
        const CommandResult outcome = local_engine.apply(sequenced(command, seq), local_out);
        if (cancel) {
            result.successful_cancel_count += outcome.has_value() ? 1U : 0U;
        } else if (outcome.has_value()) {
            const auto& new_order = std::get<NewOrder>(command);
            accepted_orders.push_back(LiveOrder{outcome->order_id, new_order.trader});
        }
        for (const Event& event : local_out.events()) {
            result.trade_count += std::holds_alternative<Trade>(event) ? 1U : 0U;
        }
        result.events.insert(result.events.end(), local_out.events().begin(),
                             local_out.events().end());
    }
    return result;
}

TEST_F(MatchingTest, IdenticalCommandSequencesProduceIdenticalEvents) {
    // ADR-0004: apply() must be a pure function of the command sequence, so two
    // independent engines fed the same 1'000 commands must diverge nowhere.
    const RandomRunResult first_run = run_random_commands();
    const RandomRunResult second_run = run_random_commands();

    ASSERT_EQ(first_run.events.size(), second_run.events.size());
    for (std::size_t i = 0; i < first_run.events.size(); ++i) {
        ASSERT_EQ(first_run.events[i], second_run.events[i]) << "first divergence at event " << i;
    }
    // A determinism check that never exercises matching or cancellation would
    // trivially "pass" without testing anything interesting; pin that this
    // fixed seed actually does both.
    EXPECT_GE(first_run.trade_count, 1U);
    EXPECT_GE(first_run.successful_cancel_count, 1U);
}

}  // namespace
}  // namespace lockstep::domain
