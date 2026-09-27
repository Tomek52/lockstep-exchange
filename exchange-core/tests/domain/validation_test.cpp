#include "lockstep/domain/validation.hpp"

#include <gtest/gtest.h>

namespace lockstep::domain {
namespace {

constexpr InstrumentSpec spec{
    .id = InstrumentId{7},
    .min_price = Price{1},
    .max_price = Price{10'000},
    .max_order_quantity = Quantity{500},
};

constexpr NewOrder limit_order(Price price, Quantity quantity) {
    return NewOrder{.trader = TraderId{1},
                    .client_order_id = ClientOrderId{1},
                    .instrument = spec.id,
                    .side = Side::Buy,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = price,
                    .quantity = quantity};
}

// Validation is constexpr: the boundary cases are pinned at compile time.
static_assert(validate(limit_order(Price{1}, Quantity{1}), spec).has_value());
static_assert(validate(limit_order(Price{10'000}, Quantity{500}), spec).has_value());
static_assert(validate(limit_order(Price{0}, Quantity{1}), spec).error() ==
              RejectReason::InvalidPrice);
static_assert(validate(limit_order(Price{10'001}, Quantity{1}), spec).error() ==
              RejectReason::InvalidPrice);

TEST(Validation, RejectsZeroAndOversizedQuantity) {
    EXPECT_EQ(validate(limit_order(Price{5}, Quantity{0}), spec).error(),
              RejectReason::InvalidQuantity);
    EXPECT_EQ(validate(limit_order(Price{5}, Quantity{501}), spec).error(),
              RejectReason::InvalidQuantity);
}

TEST(Validation, QuantityIsCheckedBeforePrice) {
    // Both invalid: the first failing check wins, deterministically.
    EXPECT_EQ(validate(limit_order(Price{0}, Quantity{0}), spec).error(),
              RejectReason::InvalidQuantity);
}

TEST(Validation, MarketOrdersMustNotCarryAPrice) {
    NewOrder market = limit_order(Price{0}, Quantity{10});
    market.type = OrderType::Market;
    EXPECT_TRUE(validate(market, spec).has_value());

    market.price = Price{5};
    EXPECT_EQ(validate(market, spec).error(), RejectReason::InvalidPrice);
}

TEST(Validation, ModifyUsesSameLimits) {
    const ModifyOrder ok{TraderId{1}, spec.id, OrderId{1}, Price{5}, Quantity{5}};
    const ModifyOrder bad{TraderId{1}, spec.id, OrderId{1}, Price{5}, Quantity{0}};
    EXPECT_TRUE(validate(ok, spec).has_value());
    EXPECT_EQ(validate(bad, spec).error(), RejectReason::InvalidQuantity);
}

}  // namespace
}  // namespace lockstep::domain
