#include "lockstep/domain/risk_state.hpp"

#include <gtest/gtest.h>

// Direct unit tests of RiskState, below ShardEngine: risk_controls_test.cpp
// covers the command handlers' event vectors, this file covers the sorted
// blocked_ set's internal invariants.
namespace lockstep::domain {
namespace {

// block() must keep blocked_ sorted regardless of insertion order, not just
// append. Kills a mutant that replaces the sorted insert with push_back:
// blocking out of order would leave the set unsorted, and is_blocked's
// binary search over an unsorted range can miss a member.
TEST(RiskStateTest, BlockOutOfOrderKeepsEveryTraderBlocked) {
    RiskState risk;
    risk.block(TraderId{3});
    risk.block(TraderId{1});
    risk.block(TraderId{2});

    EXPECT_TRUE(risk.is_blocked(TraderId{1}));
    EXPECT_TRUE(risk.is_blocked(TraderId{2}));
    EXPECT_TRUE(risk.is_blocked(TraderId{3}));
}

// unblock() of a trader that was never blocked must be a genuine no-op: it
// must not remove some other, blocked, trader. Kills a mutant that erases
// lower_bound(trader) unconditionally instead of checking *it == trader
// first (lower_bound(1) on a set containing only {2} lands on 2's entry).
TEST(RiskStateTest, UnblockOfANeverBlockedTraderLeavesOthersBlocked) {
    RiskState risk;
    risk.block(TraderId{2});

    risk.unblock(TraderId{1});  // never blocked

    EXPECT_TRUE(risk.is_blocked(TraderId{2}));
    EXPECT_FALSE(risk.is_blocked(TraderId{1}));
}

}  // namespace
}  // namespace lockstep::domain
