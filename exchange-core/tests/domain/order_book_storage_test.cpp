// Acceptance tests for docs/tasks/001-order-book-storage.md: pooled FIFO levels,
// O(1) find/cancel, and the front/reduce_front/cancel_if primitives.

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/domain/order_book.hpp"

namespace lockstep::domain {
namespace {

constexpr InstrumentId instrument{1};

RestingOrder order(
    std::uint64_t id, Side side, std::int64_t price, std::uint64_t qty, std::uint64_t trader = 1) {
    return RestingOrder{OrderId{id}, TraderId{trader}, ClientOrderId{id},
                        side,        Price{price},     Quantity{qty}};
}

class OrderBookStorageTest : public ::testing::Test {
protected:
    OrderBook book{InstrumentSpec{.id = instrument}};
    EventBuffer out;
};

TEST_F(OrderBookStorageTest, FrontIsNullOnEmptySide) {
    EXPECT_EQ(book.front(Side::Buy), nullptr);
    EXPECT_EQ(book.front(Side::Sell), nullptr);
}

TEST_F(OrderBookStorageTest, CancellingMiddleOrderPreservesFifoOfTheOthers) {
    book.rest(order(1, Side::Sell, 105, 3), out);
    book.rest(order(2, Side::Sell, 105, 4), out);
    book.rest(order(3, Side::Sell, 105, 5), out);

    ASSERT_TRUE(book.cancel(OrderId{2}, TraderId{1}, CancelReason::UserRequested, out));
    out.clear();

    const RestingOrder* first = book.front(Side::Sell);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->id, OrderId{1});
    book.reduce_front(Side::Sell, first->remaining, out);

    const RestingOrder* second = book.front(Side::Sell);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->id, OrderId{3});
    book.reduce_front(Side::Sell, second->remaining, out);

    EXPECT_EQ(book.front(Side::Sell), nullptr);
    EXPECT_EQ(book.order_count(), 0U);
}

TEST_F(OrderBookStorageTest, FindLocatesEachOfTenThousandOrdersOverManyLevels) {
    constexpr std::uint64_t levels_per_side = 100;
    constexpr std::uint64_t orders_per_level = 50;
    constexpr std::size_t total_orders = 2 * levels_per_side * orders_per_level;
    constexpr std::int64_t lowest_bid_price = 1'000;
    constexpr std::int64_t lowest_ask_price = 2'000;
    constexpr std::uint64_t trader_count = 7;
    std::vector<RestingOrder> rested;
    std::uint64_t next_id = 1;
    // Interleave levels and sides so that no level's orders are contiguous in
    // insertion order: find() must not depend on insertion locality.
    for (std::uint64_t round = 0; round < orders_per_level; ++round) {
        for (std::uint64_t level = 0; level < levels_per_side; ++level) {
            const std::int64_t bid_price = lowest_bid_price + static_cast<std::int64_t>(level);
            const std::int64_t ask_price = lowest_ask_price + static_cast<std::int64_t>(level);
            rested.push_back(
                order(next_id, Side::Buy, bid_price, round + 1, next_id % trader_count));
            ++next_id;
            rested.push_back(
                order(next_id, Side::Sell, ask_price, round + 1, next_id % trader_count));
            ++next_id;
        }
    }
    for (const RestingOrder& o : rested) {
        book.rest(o, out);
    }
    ASSERT_EQ(book.order_count(), total_orders);
    ASSERT_EQ(book.snapshot().bids.size(), levels_per_side);
    ASSERT_EQ(book.snapshot().asks.size(), levels_per_side);

    for (const RestingOrder& expected : rested) {
        const RestingOrder* found = book.find(expected.id);
        ASSERT_NE(found, nullptr) << "order " << expected.id.value();
        EXPECT_EQ(*found, expected);
    }
    EXPECT_EQ(book.find(OrderId{next_id}), nullptr);
}

TEST_F(OrderBookStorageTest, ReduceFrontPartialDecreasesOrderAndLevel) {
    book.rest(order(1, Side::Buy, 100, 10), out);
    book.rest(order(2, Side::Buy, 100, 5), out);
    out.clear();

    book.reduce_front(Side::Buy, Quantity{4}, out);

    const RestingOrder* front = book.front(Side::Buy);
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, OrderId{1});
    EXPECT_EQ(front->remaining, Quantity{6});
    EXPECT_EQ(book.find(OrderId{1})->remaining, Quantity{6});
    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{11});
    EXPECT_EQ(book.order_count(), 2U);

    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(std::get<BookLevelChanged>(out.events()[0]),
              (BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{11}}));
}

TEST_F(OrderBookStorageTest, ReduceFrontFullRemovesOrderThenLevel) {
    book.rest(order(1, Side::Sell, 105, 3), out);
    book.rest(order(2, Side::Sell, 105, 4), out);
    book.rest(order(3, Side::Sell, 107, 9), out);
    out.clear();

    // Full fill of the first order: the level survives with the second order.
    book.reduce_front(Side::Sell, Quantity{3}, out);
    EXPECT_EQ(book.find(OrderId{1}), nullptr);
    ASSERT_NE(book.front(Side::Sell), nullptr);
    EXPECT_EQ(book.front(Side::Sell)->id, OrderId{2});
    EXPECT_EQ(book.best_price(Side::Sell), Price{105});
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(std::get<BookLevelChanged>(out.events()[0]),
              (BookLevelChanged{instrument, Side::Sell, Price{105}, Quantity{4}}));
    out.clear();

    // Full fill of the last order at 105: the level disappears.
    book.reduce_front(Side::Sell, Quantity{4}, out);
    EXPECT_EQ(book.find(OrderId{2}), nullptr);
    EXPECT_EQ(book.best_price(Side::Sell), Price{107});
    EXPECT_EQ(book.quantity_at(Side::Sell, Price{105}), Quantity{0});
    ASSERT_NE(book.front(Side::Sell), nullptr);
    EXPECT_EQ(book.front(Side::Sell)->id, OrderId{3});
    EXPECT_EQ(book.snapshot().asks.size(), 1U);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(std::get<BookLevelChanged>(out.events()[0]),
              (BookLevelChanged{instrument, Side::Sell, Price{105}, Quantity{0}}));
}

TEST_F(OrderBookStorageTest, CancelIfByTraderRemovesMatchesInDocumentedOrder) {
    constexpr TraderId victim{7};
    // Rest in an order that differs from the documented output order, so the
    // test catches iteration by insertion order or by id.
    book.rest(order(1, Side::Sell, 106, 1, 7), out);
    book.rest(order(2, Side::Buy, 99, 2, 7), out);
    book.rest(order(3, Side::Buy, 100, 3, 1), out);
    book.rest(order(4, Side::Sell, 105, 4, 7), out);
    book.rest(order(5, Side::Buy, 100, 5, 7), out);
    book.rest(order(6, Side::Buy, 100, 6, 7), out);
    book.rest(order(7, Side::Sell, 105, 7, 2), out);
    out.clear();

    const auto owned_by_victim = [victim](const RestingOrder& o) { return o.trader == victim; };
    const std::size_t cancelled = book.cancel_if(owned_by_victim, CancelReason::KillSwitch, out);

    EXPECT_EQ(cancelled, 5U);
    // Bids best-to-worst (100 then 99), then asks best-to-worst (105 then 106),
    // FIFO within a level; each touched level reports its new total once,
    // after its cancellations.
    const std::vector<Event> expected{
        OrderCancelled{OrderId{5}, victim, instrument, Quantity{5}, CancelReason::KillSwitch},
        OrderCancelled{OrderId{6}, victim, instrument, Quantity{6}, CancelReason::KillSwitch},
        BookLevelChanged{instrument, Side::Buy, Price{100}, Quantity{3}},
        OrderCancelled{OrderId{2}, victim, instrument, Quantity{2}, CancelReason::KillSwitch},
        BookLevelChanged{instrument, Side::Buy, Price{99}, Quantity{0}},
        OrderCancelled{OrderId{4}, victim, instrument, Quantity{4}, CancelReason::KillSwitch},
        BookLevelChanged{instrument, Side::Sell, Price{105}, Quantity{7}},
        OrderCancelled{OrderId{1}, victim, instrument, Quantity{1}, CancelReason::KillSwitch},
        BookLevelChanged{instrument, Side::Sell, Price{106}, Quantity{0}},
    };
    EXPECT_EQ(std::vector<Event>(out.events().begin(), out.events().end()), expected);

    EXPECT_EQ(book.order_count(), 2U);
    EXPECT_NE(book.find(OrderId{3}), nullptr);
    EXPECT_NE(book.find(OrderId{7}), nullptr);
    for (const std::uint64_t id : {1U, 2U, 4U, 5U, 6U}) {
        EXPECT_EQ(book.find(OrderId{id}), nullptr) << "order " << id;
    }
    const BookSnapshot snap = book.snapshot();
    ASSERT_EQ(snap.bids.size(), 1U);
    EXPECT_EQ(snap.bids[0], (LevelView{Price{100}, Quantity{3}, 1}));
    ASSERT_EQ(snap.asks.size(), 1U);
    EXPECT_EQ(snap.asks[0], (LevelView{Price{105}, Quantity{7}, 1}));
}

TEST_F(OrderBookStorageTest, CancelIfMatchingNothingEmitsNothing) {
    book.rest(order(1, Side::Buy, 100, 1, 1), out);
    out.clear();

    const auto match_none = [](const RestingOrder&) { return false; };
    EXPECT_EQ(book.cancel_if(match_none, CancelReason::TraderBlocked, out), 0U);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(book.order_count(), 1U);
}

TEST_F(OrderBookStorageTest, SlotsAreReusedAfterCancel) {
    constexpr std::uint64_t orders_per_round = 1'000;
    constexpr int rounds = 10;
    constexpr std::int64_t lowest_price = 100;
    constexpr std::uint64_t price_levels = 10;
    std::size_t capacity_after_first_round = 0;
    std::uint64_t next_id = 1;

    for (int round = 0; round < rounds; ++round) {
        const std::uint64_t first_id = next_id;
        for (std::uint64_t i = 0; i < orders_per_round; ++i) {
            const std::int64_t price = lowest_price + static_cast<std::int64_t>(i % price_levels);
            book.rest(order(next_id, i % 2 == 0 ? Side::Buy : Side::Sell, price, 1), out);
            ++next_id;
        }
        for (std::uint64_t id = first_id; id < next_id; ++id) {
            ASSERT_TRUE(book.cancel(OrderId{id}, TraderId{1}, CancelReason::UserRequested, out));
        }
        out.clear();
        ASSERT_EQ(book.order_count(), 0U);

        if (round == 0) {
            capacity_after_first_round = book.pool_capacity();
            EXPECT_GE(capacity_after_first_round, orders_per_round);
        } else {
            EXPECT_EQ(book.pool_capacity(), capacity_after_first_round) << "round " << round;
        }
    }
}

}  // namespace
}  // namespace lockstep::domain
