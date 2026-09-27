#include "lockstep/concurrency/idle_strategy.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace lockstep::concurrency {
namespace {

TEST(BackoffIdle, EscalatesWhileIdleAndResetsOnWork) {
    BackoffIdle idle{{.spins = 2, .yields = 2, .sleep = std::chrono::microseconds{1}}};
    for (int i = 0; i < 3; ++i) {
        idle.idle(0);
    }
    EXPECT_EQ(idle.idle_iterations(), 3U);

    idle.idle(1);
    EXPECT_EQ(idle.idle_iterations(), 0U);
}

TEST(BackoffIdle, SaturatesInSleepPhase) {
    BackoffIdle idle{{.spins = 1, .yields = 1, .sleep = std::chrono::microseconds{1}}};
    for (int i = 0; i < 10; ++i) {
        idle.idle(0);
    }
    EXPECT_EQ(idle.idle_iterations(), 2U);
}

}  // namespace
}  // namespace lockstep::concurrency
