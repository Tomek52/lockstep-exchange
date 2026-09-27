#include "lockstep/app/router.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace lockstep::app {
namespace {

using domain::InstrumentId;
using domain::InstrumentSpec;
using domain::ShardId;

TEST(Router, DealsInstrumentsRoundRobinBySortedId) {
    // Config order must not matter: assignment is a function of the id set.
    const std::vector<InstrumentSpec> instruments{
        {.id = InstrumentId{30}}, {.id = InstrumentId{10}}, {.id = InstrumentId{20}}};
    const Router router = Router::round_robin(instruments, 2);

    EXPECT_EQ(router.shard_count(), 2U);
    EXPECT_EQ(router.shard_for(InstrumentId{10}), ShardId{0});
    EXPECT_EQ(router.shard_for(InstrumentId{20}), ShardId{1});
    EXPECT_EQ(router.shard_for(InstrumentId{30}), ShardId{0});
    EXPECT_EQ(router.instruments_of(ShardId{0}).size(), 2U);
    EXPECT_FALSE(router.shard_for(InstrumentId{99}).has_value());
}

TEST(Router, InstrumentOfDistinguishesOrderAndRiskCommands) {
    EXPECT_EQ(instrument_of(
                  domain::CancelOrder{domain::TraderId{1}, InstrumentId{5}, domain::OrderId{1}}),
              InstrumentId{5});
    EXPECT_FALSE(instrument_of(domain::KillSwitch{}).has_value());
    EXPECT_FALSE(instrument_of(domain::RiskLinkStatus{}).has_value());
}

}  // namespace
}  // namespace lockstep::app
