#include <concepts>
#include <unordered_set>

#include <gtest/gtest.h>

#include "lockstep/domain/types.hpp"

namespace lockstep::domain {
namespace {

template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };

template <typename A, typename B>
concept Comparable = requires(A a, B b) { a < b; };

// The whole point of strong types: mixing units must not compile.
static_assert(!Addable<Price, Quantity>);
static_assert(!Comparable<Price, Quantity>);
static_assert(!Comparable<OrderId, ClientOrderId>);
static_assert(!std::convertible_to<std::int64_t, Price>, "construction must be explicit");
static_assert(!std::convertible_to<Price, std::int64_t>);
// Identifiers are not arithmetic.
static_assert(!Addable<OrderId, OrderId>);
// Arithmetic keeps the most-derived type (deducing this, not CRTP).
static_assert(std::same_as<decltype(Price{1} + Price{2}), Price>);
static_assert((Quantity{5} - Quantity{3}) == Quantity{2});
static_assert(Price{1} < Price{2});

TEST(StrongInt, CompoundAssignmentUpdatesInPlace) {
    Quantity q{10};
    q += Quantity{5};
    q -= Quantity{3};
    EXPECT_EQ(q, Quantity{12});
}

TEST(StrongInt, HashableForUnorderedContainers) {
    std::unordered_set<OrderId, StrongIntHash> ids{OrderId{1}, OrderId{2}, OrderId{1}};
    EXPECT_EQ(ids.size(), 2U);
    EXPECT_TRUE(ids.contains(OrderId{2}));
}

TEST(Side, OppositeIsAnInvolution) {
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(opposite(Side::Sell)), Side::Sell);
}

}  // namespace
}  // namespace lockstep::domain
