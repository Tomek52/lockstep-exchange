#include "lockstep/domain/order_book.hpp"

#include <variant>

#include <gtest/gtest.h>

namespace lockstep::domain {
namespace {

constexpr InstrumentId instrument{1};

RestingOrder order(
    std::uint64_t id, Side side, std::int64_t price, std::uint64_t qty, std::uint64_t trader = 1) {
    return RestingOrder{OrderId{id}, TraderId{trader}, ClientOrderId{id},
                        side,        Price{price},     Quantity{qty}};
}

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book{InstrumentSpec{.id = instrument}};
    EventBuffer out;
};

// [utest->req~order-book-storage.existing-book-tests-pass-unmodified~1]
TEST_F(OrderBookTest, EmptyBookHasNoBestPrice) {
    EXPECT_FALSE(book.best_price(Side::Buy).has_value());
    EXPECT_FALSE(book.best_price(Side::Sell).has_value());
    EXPECT_EQ(book.order_count(), 0U);
}

// [utest->req~order-book-storage.existing-book-tests-pass-unmodified~1]
TEST_F(OrderBookTest, BestBidIsHighestAndBestAskIsLowest) {
    book.rest(order(1, Side::Buy, 100, 5), out);
    book.rest(order(2, Side::Buy, 102, 5), out);
    book.rest(order(3, Side::Sell, 110, 5), out);
    book.rest(order(4, Side::Sell, 105, 5), out);

    EXPECT_EQ(book.best_price(Side::Buy), Price{102});
    EXPECT_EQ(book.best_price(Side::Sell), Price{105});
}

// [utest->req~order-book-storage.existing-book-tests-pass-unmodified~1]
TEST_F(OrderBookTest, RestingAggregatesLevelAndEmitsLevelUpdate) {
    book.rest(order(1, Side::Buy, 100, 5), out);
    book.rest(order(2, Side::Buy, 100, 7), out);

    EXPECT_EQ(book.quantity_at(Side::Buy, Price{100}), Quantity{12});
    ASSERT_EQ(out.size(), 2U);
    const auto& last = std::get<BookLevelChanged>(out.events().back());
    EXPECT_EQ(last.quantity, Quantity{12});
    EXPECT_EQ(last.side, Side::Buy);
}

// [utest->req~order-book-storage.existing-book-tests-pass-unmodified~1]
TEST_F(OrderBookTest, SnapshotListsBestLevelsFirst) {
    book.rest(order(1, Side::Buy, 100, 5), out);
    book.rest(order(2, Side::Buy, 101, 1), out);
    book.rest(order(3, Side::Buy, 101, 2), out);
    book.rest(order(4, Side::Sell, 103, 4), out);

    const BookSnapshot snap = book.snapshot();
    EXPECT_EQ(snap.instrument, instrument);
    ASSERT_EQ(snap.bids.size(), 2U);
    EXPECT_EQ(snap.bids[0], (LevelView{Price{101}, Quantity{3}, 2}));
    EXPECT_EQ(snap.bids[1], (LevelView{Price{100}, Quantity{5}, 1}));
    ASSERT_EQ(snap.asks.size(), 1U);
    EXPECT_EQ(snap.asks[0], (LevelView{Price{103}, Quantity{4}, 1}));
}

// [utest->req~order-book-storage.existing-book-tests-pass-unmodified~1]
TEST_F(OrderBookTest, CancelRemovesOrderAndEmptyLevel) {
    book.rest(order(1, Side::Sell, 105, 5), out);
    out.clear();

    const auto cancelled = book.cancel(OrderId{1}, TraderId{1}, CancelReason::UserRequested, out);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(*cancelled, Quantity{5});
    EXPECT_FALSE(book.best_price(Side::Sell).has_value());
    EXPECT_EQ(book.find(OrderId{1}), nullptr);

    ASSERT_EQ(out.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<OrderCancelled>(out.events()[0]));
    EXPECT_EQ(std::get<BookLevelChanged>(out.events()[1]).quantity, Quantity{0});
}

// [utest->req~order-book-storage.existing-book-tests-pass-unmodified~1]
TEST_F(OrderBookTest, ClientOrderIndexTracksRestingOrdersExactlyNotCumulatively) {
    // Regression for task-003 review: the duplicate-client-id index must stay
    // in sync with what is actually resting, not just grow whenever a new
    // (trader, client_order_id) pair is seen. Resting and cancelling many
    // orders with distinct client ids, all for one trader, reproduces the
    // reported bug (resting 0, index 100000) at a size this test can run
    // quickly; the fix (task-003 review) makes remove() erase it exactly.
    constexpr std::uint64_t total_orders = 5'000;
    for (std::uint64_t i = 1; i <= total_orders; ++i) {
        book.rest(order(i, Side::Buy, 100, 1, /*trader=*/1), out);
    }
    ASSERT_EQ(book.order_count(), total_orders);
    EXPECT_EQ(book.client_order_index_size(), total_orders);

    for (std::uint64_t i = 1; i <= total_orders; ++i) {
        ASSERT_TRUE(
            book.cancel(OrderId{i}, TraderId{1}, CancelReason::UserRequested, out).has_value());
    }
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.client_order_index_size(), 0U);
}

// [utest->req~order-book-storage.existing-book-tests-pass-unmodified~1]
TEST_F(OrderBookTest, CancelRejectsUnknownOrderAndForeignTrader) {
    book.rest(order(1, Side::Buy, 100, 5, /*trader=*/1), out);
    EXPECT_EQ(book.cancel(OrderId{99}, TraderId{1}, CancelReason::UserRequested, out).error(),
              RejectReason::UnknownOrder);
    EXPECT_EQ(book.cancel(OrderId{1}, TraderId{2}, CancelReason::UserRequested, out).error(),
              RejectReason::NotOrderOwner);
    EXPECT_NE(book.find(OrderId{1}), nullptr);
}

}  // namespace
}  // namespace lockstep::domain
