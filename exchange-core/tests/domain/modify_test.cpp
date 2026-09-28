#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/domain/shard_engine.hpp"

namespace lockstep::domain {
namespace {

constexpr InstrumentId instrument{9};

SequencedCommand sequenced(Command command, std::uint64_t seq = 1) {
    return SequencedCommand{SequenceNumber{seq}, Timestamp{1'000 * static_cast<std::int64_t>(seq)},
                            std::move(command)};
}

NewOrder limit_order(Side side,
                     std::int64_t price,
                     std::uint64_t qty,
                     TraderId trader = TraderId{1},
                     ClientOrderId client_order_id = ClientOrderId{1}) {
    return NewOrder{.trader = trader,
                    .client_order_id = client_order_id,
                    .instrument = instrument,
                    .side = side,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = Price{price},
                    .quantity = Quantity{qty}};
}

ModifyOrder modify(OrderId id,
                   std::int64_t new_price,
                   std::uint64_t new_quantity,
                   TraderId trader = TraderId{1}) {
    return ModifyOrder{trader, instrument, id, Price{new_price}, Quantity{new_quantity}};
}

// Every acceptance criterion asserts the complete event sequence, not just a
// prefix of it, so a copy of the buffer is compared against an expected
// vector via each event alternative's defaulted operator==.
std::vector<Event> events_vector(const EventBuffer& out) {
    return {out.events().begin(), out.events().end()};
}

class ModifyTest : public ::testing::Test {
protected:
    ShardEngine engine{ShardConfig{.shard = ShardId{4}, .instruments = {{.id = instrument}}}};
    EventBuffer out;
};

TEST_F(ModifyTest, ReducingQuantityAtSamePriceKeepsPriorityOnBuy) {
    const auto first = engine.apply(sequenced(limit_order(Side::Buy, 100, 10), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();
    const auto second =
        engine.apply(sequenced(limit_order(Side::Buy, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(second.has_value());
    const OrderId second_id = second->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 100, 6), 3), out);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->order_id, first_id);
    const std::vector<Event> expected{
        OrderModified{first_id, TraderId{1}, instrument, Price{100}, Quantity{6}, true},
        // Level total after the shrink: first order's new 6 + second's untouched 5.
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{11}},
    };
    EXPECT_EQ(events_vector(out), expected);

    const RestingOrder* front = engine.book(instrument)->front(Side::Buy);
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, first_id);
    EXPECT_EQ(front->remaining, Quantity{6});
    const RestingOrder* second_untouched = engine.book(instrument)->find(second_id);
    ASSERT_NE(second_untouched, nullptr);
    EXPECT_EQ(second_untouched->remaining, Quantity{5});
}

TEST_F(ModifyTest, ReducingQuantityAtSamePriceKeepsPriorityOnSell) {
    const auto first = engine.apply(sequenced(limit_order(Side::Sell, 100, 10), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();
    const auto second =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(second.has_value());
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 100, 3), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{first_id, TraderId{1}, instrument, Price{100}, Quantity{3}, true},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{8}},
    };
    EXPECT_EQ(events_vector(out), expected);

    const RestingOrder* front = engine.book(instrument)->front(Side::Sell);
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, first_id);
    EXPECT_EQ(front->remaining, Quantity{3});
}

TEST_F(ModifyTest, SameQuantityAtSamePriceIsANoOp) {
    const auto first = engine.apply(sequenced(limit_order(Side::Buy, 100, 10), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 100, 10), 2), out);

    ASSERT_TRUE(result.has_value());
    // No book mutation, so no BookLevelChanged: only the acknowledgement.
    const std::vector<Event> expected{
        OrderModified{first_id, TraderId{1}, instrument, Price{100}, Quantity{10}, true},
    };
    EXPECT_EQ(events_vector(out), expected);
    EXPECT_EQ(engine.book(instrument)->find(first_id)->remaining, Quantity{10});
}

TEST_F(ModifyTest, IncreasingQuantityAtSamePriceLosesPriorityOnBuy) {
    const auto first = engine.apply(sequenced(limit_order(Side::Buy, 100, 5), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();
    const auto second =
        engine.apply(sequenced(limit_order(Side::Buy, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(second.has_value());
    const OrderId second_id = second->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 100, 8), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{first_id, TraderId{1}, instrument, Price{100}, Quantity{8}, false},
        // take() removing the order leaves only the second order at the level.
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{5}},
        // No crossing order on the sell side, so it all rests, at the tail.
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{13}},
    };
    EXPECT_EQ(events_vector(out), expected);

    // Now behind the second order in the FIFO: it is the front, not the first.
    const RestingOrder* front = engine.book(instrument)->front(Side::Buy);
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, second_id);
    const RestingOrder* moved = engine.book(instrument)->find(first_id);
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->remaining, Quantity{8});
}

TEST_F(ModifyTest, IncreasingQuantityAtSamePriceLosesPriorityOnSell) {
    const auto first = engine.apply(sequenced(limit_order(Side::Sell, 100, 5), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();
    const auto second =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(second.has_value());
    const OrderId second_id = second->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 100, 8), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{first_id, TraderId{1}, instrument, Price{100}, Quantity{8}, false},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{5}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{13}},
    };
    EXPECT_EQ(events_vector(out), expected);

    const RestingOrder* front = engine.book(instrument)->front(Side::Sell);
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, second_id);
}

TEST_F(ModifyTest, PriceChangeToACrossingPriceTradesImmediatelyOnBuy) {
    const auto maker =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 5, TraderId{2}), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();
    const auto resting = engine.apply(sequenced(limit_order(Side::Buy, 90, 5), 2), out);
    ASSERT_TRUE(resting.has_value());
    const OrderId order_id = resting->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(order_id, 100, 5), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{order_id, TraderId{1}, instrument, Price{100}, Quantity{5}, false},
        BookLevelChanged{instrument, Side::Buy, Price{90}, Quantity{0}},
        Trade{instrument, Price{100}, Quantity{5}, Side::Buy, maker_id, TraderId{2}, order_id,
              TraderId{1}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
    };
    EXPECT_EQ(events_vector(out), expected);

    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Buy).has_value());
    EXPECT_FALSE(engine.book(instrument)->best_price(Side::Sell).has_value());
}

TEST_F(ModifyTest, PriceChangeToACrossingPriceTradesImmediatelyOnSell) {
    const auto maker = engine.apply(sequenced(limit_order(Side::Buy, 100, 5, TraderId{2}), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();
    const auto resting = engine.apply(sequenced(limit_order(Side::Sell, 110, 5), 2), out);
    ASSERT_TRUE(resting.has_value());
    const OrderId order_id = resting->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(order_id, 100, 5), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{order_id, TraderId{1}, instrument, Price{100}, Quantity{5}, false},
        BookLevelChanged{instrument, Side::Sell, Price{110}, Quantity{0}},
        Trade{instrument, Price{100}, Quantity{5}, Side::Sell, maker_id, TraderId{2}, order_id,
              TraderId{1}},
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{0}},
    };
    EXPECT_EQ(events_vector(out), expected);
}

TEST_F(ModifyTest, PartialFillOnReplaceRestsTheRemainderAtTheNewPrice) {
    const auto maker =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 3, TraderId{2}), 1), out);
    ASSERT_TRUE(maker.has_value());
    const OrderId maker_id = maker->order_id;
    out.clear();
    const auto resting = engine.apply(sequenced(limit_order(Side::Buy, 90, 10), 2), out);
    ASSERT_TRUE(resting.has_value());
    const OrderId order_id = resting->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(order_id, 100, 10), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{order_id, TraderId{1}, instrument, Price{100}, Quantity{10}, false},
        BookLevelChanged{instrument, Side::Buy, Price{90}, Quantity{0}},
        Trade{instrument, Price{100}, Quantity{3}, Side::Buy, maker_id, TraderId{2}, order_id,
              TraderId{1}},
        BookLevelChanged{instrument, Side::Sell, Price{100}, Quantity{0}},
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{7}},
    };
    EXPECT_EQ(events_vector(out), expected);

    const RestingOrder* remainder = engine.book(instrument)->find(order_id);
    ASSERT_NE(remainder, nullptr);
    EXPECT_EQ(remainder->price, Price{100});
    EXPECT_EQ(remainder->remaining, Quantity{7});
}

TEST_F(ModifyTest, PriceChangeWithSmallerQuantityNonCrossingLosesPriorityOnBuy) {
    // Regression for task-003 review: a price change must always cancel and
    // replace, even when the new quantity is also smaller than the current
    // remaining. `resting->price` differs from `modify.new_price` here, so a
    // condition that only looks at the quantity comparison (survived mutant:
    // `(new_price == price || new_quantity < remaining) && new_quantity <=
    // remaining`) would wrongly take the keep-priority path and never move
    // the order to price 95.
    const auto moved = engine.apply(
        sequenced(limit_order(Side::Buy, 90, 10, TraderId{1}, ClientOrderId{1}), 1), out);
    ASSERT_TRUE(moved.has_value());
    const OrderId moved_id = moved->order_id;
    out.clear();
    const auto already_there = engine.apply(
        sequenced(limit_order(Side::Buy, 95, 5, TraderId{2}, ClientOrderId{1}), 2), out);
    ASSERT_TRUE(already_there.has_value());
    const OrderId already_there_id = already_there->order_id;
    out.clear();

    // Smaller quantity (6 < 10) at a different price (95 != 90): no resting
    // sell order to cross, so this rests rather than trading.
    const auto result = engine.apply(sequenced(modify(moved_id, 95, 6), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{moved_id, TraderId{1}, instrument, Price{95}, Quantity{6}, false},
        BookLevelChanged{instrument, Side::Buy, Price{90}, Quantity{0}},
        BookLevelChanged{instrument, Side::Buy, Price{95}, Quantity{11}},
    };
    EXPECT_EQ(events_vector(out), expected);

    EXPECT_EQ(engine.book(instrument)->best_price(Side::Buy), Price{95});
    const RestingOrder* front = engine.book(instrument)->front(Side::Buy);
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, already_there_id);
    const RestingOrder* moved_order = engine.book(instrument)->find(moved_id);
    ASSERT_NE(moved_order, nullptr);
    EXPECT_EQ(moved_order->price, Price{95});
    EXPECT_EQ(moved_order->remaining, Quantity{6});
}

TEST_F(ModifyTest, PriceChangeWithSmallerQuantityNonCrossingLosesPriorityOnSell) {
    // Mirror of PriceChangeWithSmallerQuantityNonCrossingLosesPriorityOnBuy.
    const auto moved = engine.apply(
        sequenced(limit_order(Side::Sell, 110, 10, TraderId{1}, ClientOrderId{1}), 1), out);
    ASSERT_TRUE(moved.has_value());
    const OrderId moved_id = moved->order_id;
    out.clear();
    const auto already_there = engine.apply(
        sequenced(limit_order(Side::Sell, 105, 5, TraderId{2}, ClientOrderId{1}), 2), out);
    ASSERT_TRUE(already_there.has_value());
    const OrderId already_there_id = already_there->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(moved_id, 105, 6), 3), out);

    ASSERT_TRUE(result.has_value());
    const std::vector<Event> expected{
        OrderModified{moved_id, TraderId{1}, instrument, Price{105}, Quantity{6}, false},
        BookLevelChanged{instrument, Side::Sell, Price{110}, Quantity{0}},
        BookLevelChanged{instrument, Side::Sell, Price{105}, Quantity{11}},
    };
    EXPECT_EQ(events_vector(out), expected);

    EXPECT_EQ(engine.book(instrument)->best_price(Side::Sell), Price{105});
    const RestingOrder* front = engine.book(instrument)->front(Side::Sell);
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, already_there_id);
    const RestingOrder* moved_order = engine.book(instrument)->find(moved_id);
    ASSERT_NE(moved_order, nullptr);
    EXPECT_EQ(moved_order->price, Price{105});
    EXPECT_EQ(moved_order->remaining, Quantity{6});
}

TEST_F(ModifyTest, ModifyByNonOwnerIsRejectedAndLeavesTheBookUnchanged) {
    const auto first = engine.apply(sequenced(limit_order(Side::Buy, 100, 10), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 100, 5, TraderId{2}), 2), out);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RejectReason::NotOrderOwner);
    EXPECT_TRUE(out.empty());
    const RestingOrder* unchanged = engine.book(instrument)->find(first_id);
    ASSERT_NE(unchanged, nullptr);
    EXPECT_EQ(unchanged->remaining, Quantity{10});
}

TEST_F(ModifyTest, ModifyOfAnUnknownOrderIsRejected) {
    const auto result = engine.apply(sequenced(modify(OrderId{99999}, 100, 5)), out);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RejectReason::UnknownOrder);
    EXPECT_TRUE(out.empty());
}

TEST_F(ModifyTest, ModifyWithInvalidPriceIsRejected) {
    const auto first = engine.apply(sequenced(limit_order(Side::Buy, 100, 10), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 0, 5), 2), out);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RejectReason::InvalidPrice);
    EXPECT_TRUE(out.empty());
}

TEST_F(ModifyTest, ModifyWithInvalidQuantityIsRejected) {
    const auto first = engine.apply(sequenced(limit_order(Side::Buy, 100, 10), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();

    const auto result = engine.apply(sequenced(modify(first_id, 100, 0), 2), out);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RejectReason::InvalidQuantity);
    EXPECT_TRUE(out.empty());
}

TEST_F(ModifyTest, NewOrderWithDuplicateClientIdWhileRestingIsRejected) {
    const auto first = engine.apply(
        sequenced(limit_order(Side::Buy, 100, 5, TraderId{1}, ClientOrderId{7}), 1), out);
    ASSERT_TRUE(first.has_value());
    out.clear();

    const auto duplicate = engine.apply(
        sequenced(limit_order(Side::Sell, 110, 5, TraderId{1}, ClientOrderId{7}), 2), out);

    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error(), RejectReason::DuplicateClientOrderId);
    EXPECT_TRUE(out.empty());
    // A different trader may use the same client id: the scope is per-trader.
    const auto other_trader = engine.apply(
        sequenced(limit_order(Side::Sell, 110, 5, TraderId{2}, ClientOrderId{7}), 3), out);
    EXPECT_TRUE(other_trader.has_value());
}

TEST_F(ModifyTest, DuplicateClientIdStillRejectedAfterModifyMovesTheOrder) {
    // Regression for task-003 review: a cancel/replace modify keeps the same
    // OrderId and client_order_id, so the id must still read as in-use after
    // the order moves to a new price, not just while it sits at its original
    // one.
    const auto first = engine.apply(
        sequenced(limit_order(Side::Buy, 90, 5, TraderId{1}, ClientOrderId{7}), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();

    const auto modify_result = engine.apply(sequenced(modify(first_id, 95, 5), 2), out);
    ASSERT_TRUE(modify_result.has_value());
    ASSERT_NE(engine.book(instrument)->find(first_id), nullptr);
    out.clear();

    const auto duplicate = engine.apply(
        sequenced(limit_order(Side::Sell, 110, 5, TraderId{1}, ClientOrderId{7}), 3), out);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error(), RejectReason::DuplicateClientOrderId);
}

TEST_F(ModifyTest, DuplicateClientIdIsRejectedOnEveryResubmission) {
    // Regression for task-003 review: a rejected NewOrder must not disturb
    // the index, or a rejected duplicate could free the very id it collided
    // with.
    const auto first = engine.apply(
        sequenced(limit_order(Side::Buy, 100, 5, TraderId{1}, ClientOrderId{7}), 1), out);
    ASSERT_TRUE(first.has_value());
    out.clear();

    const auto duplicate_once = engine.apply(
        sequenced(limit_order(Side::Sell, 110, 5, TraderId{1}, ClientOrderId{7}), 2), out);
    ASSERT_FALSE(duplicate_once.has_value());
    EXPECT_EQ(duplicate_once.error(), RejectReason::DuplicateClientOrderId);

    const auto duplicate_twice = engine.apply(
        sequenced(limit_order(Side::Sell, 110, 5, TraderId{1}, ClientOrderId{7}), 3), out);
    ASSERT_FALSE(duplicate_twice.has_value());
    EXPECT_EQ(duplicate_twice.error(), RejectReason::DuplicateClientOrderId);
}

TEST_F(ModifyTest, ClientIdIsReusableAfterTheOriginalOrderIsCancelled) {
    const auto first = engine.apply(
        sequenced(limit_order(Side::Buy, 100, 5, TraderId{1}, ClientOrderId{7}), 1), out);
    ASSERT_TRUE(first.has_value());
    const OrderId first_id = first->order_id;
    out.clear();

    const auto cancel_result =
        engine.apply(sequenced(CancelOrder{TraderId{1}, instrument, first_id}, 2), out);
    ASSERT_TRUE(cancel_result.has_value());
    out.clear();

    const auto reused = engine.apply(
        sequenced(limit_order(Side::Sell, 110, 5, TraderId{1}, ClientOrderId{7}), 3), out);
    EXPECT_TRUE(reused.has_value());
}

TEST_F(ModifyTest, ClientIdIsReusableAfterTheOriginalOrderIsFullyFilled) {
    const auto resting = engine.apply(
        sequenced(limit_order(Side::Sell, 100, 5, TraderId{1}, ClientOrderId{7}), 1), out);
    ASSERT_TRUE(resting.has_value());
    out.clear();

    const auto fill = engine.apply(sequenced(limit_order(Side::Buy, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(fill.has_value());
    out.clear();

    const auto reused = engine.apply(
        sequenced(limit_order(Side::Buy, 90, 5, TraderId{1}, ClientOrderId{7}), 3), out);
    EXPECT_TRUE(reused.has_value());
}

TEST_F(ModifyTest, ClientIdIsReusableAfterAModifyCancelReplaceFullyFillsTheOrder) {
    // The duplicate-id index is only ever written by on(NewOrder)'s rest();
    // this exercises that a cancel/replace full fill (which never calls
    // track_client_order) still frees the id, via is_duplicate_client_order's
    // liveness check on the unchanged OrderId rather than a removal here.
    const auto resting = engine.apply(
        sequenced(limit_order(Side::Buy, 90, 5, TraderId{1}, ClientOrderId{7}), 1), out);
    ASSERT_TRUE(resting.has_value());
    const OrderId order_id = resting->order_id;
    out.clear();

    const auto maker =
        engine.apply(sequenced(limit_order(Side::Sell, 100, 5, TraderId{2}), 2), out);
    ASSERT_TRUE(maker.has_value());
    out.clear();

    // Modify to a crossing price: cancel/replace fully fills, so order_id
    // never rests again.
    const auto result = engine.apply(sequenced(modify(order_id, 100, 5), 3), out);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(engine.book(instrument)->find(order_id), nullptr);
    out.clear();

    const auto reused = engine.apply(
        sequenced(limit_order(Side::Buy, 90, 5, TraderId{1}, ClientOrderId{7}), 4), out);
    EXPECT_TRUE(reused.has_value());
}

TEST_F(ModifyTest, DuplicateClientIdCheckSpansEveryBookInTheShard) {
    constexpr InstrumentId other_instrument{10};
    ShardEngine multi_instrument_engine{ShardConfig{
        .shard = ShardId{4}, .instruments = {{.id = instrument}, {.id = other_instrument}}}};

    const auto first = multi_instrument_engine.apply(
        sequenced(limit_order(Side::Buy, 100, 5, TraderId{1}, ClientOrderId{7}), 1), out);
    ASSERT_TRUE(first.has_value());
    out.clear();

    NewOrder second_book_order = limit_order(Side::Buy, 100, 5, TraderId{1}, ClientOrderId{7});
    second_book_order.instrument = other_instrument;
    const auto duplicate = multi_instrument_engine.apply(sequenced(second_book_order, 2), out);

    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error(), RejectReason::DuplicateClientOrderId);
}

}  // namespace
}  // namespace lockstep::domain
