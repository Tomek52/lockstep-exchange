#include "lockstep/domain/checked_math.hpp"

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace lockstep::domain {
namespace {

constexpr auto i64_max = std::numeric_limits<std::int64_t>::max();
constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();

// constexpr evaluation: the builtins are usable at compile time.
static_assert(checked_add<std::int64_t>(2, 3) == 5);
static_assert(!checked_add<std::int64_t>(i64_max, 1).has_value());
static_assert(!checked_sub<std::uint64_t>(0, 1).has_value());
static_assert(!checked_mul<std::uint64_t>(u64_max / 2 + 1, 2).has_value());

TEST(CheckedMath, NotionalOfMaxPriceAndQuantityFitsInt64) {
    // Price * quantity for the default InstrumentSpec limits (1e9 ticks, 1e6 lots)
    // must not overflow: this is the invariant the unchecked Additive ops rely on.
    const auto notional = checked_mul<std::int64_t>(1'000'000'000, 1'000'000);
    ASSERT_TRUE(notional.has_value());
    EXPECT_EQ(*notional, 1'000'000'000'000'000);
}

TEST(CheckedMath, DetectsSignedOverflowAtRuntime) {
    volatile std::int64_t big = i64_max;  // defeat constant folding
    EXPECT_FALSE(checked_mul<std::int64_t>(big, 2).has_value());
    EXPECT_FALSE(checked_sub<std::int64_t>(-big, 2).has_value());
}

}  // namespace
}  // namespace lockstep::domain
