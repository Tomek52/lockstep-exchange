// Tests specific to SpscQueue: stress (task spec acceptance criterion 2),
// differential against MutexQueue, lifetime, capacity rounding, and the
// wrap-around / move-only edge cases called out in task 005.
#include "lockstep/concurrency/spsc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/concurrency/mutex_queue.hpp"

namespace lockstep::concurrency {
namespace {

// Move-only, non-default-constructible: exercises construct_at/destroy_at in
// the slot storage rather than a type that could be default-constructed in
// place and assigned.
struct MoveOnly {
    explicit MoveOnly(int v) : value{v} {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    ~MoveOnly() = default;

    int value;
};

TEST(SpscQueueTest, StressOneProducerOneConsumerFiveMillionSequentialInts) {
    constexpr std::size_t total = 5'000'000;
    SpscQueue<std::size_t> queue{1024};

    // The stop token lets a failed assertion below end the test instead of
    // leaving the producer spinning on a full queue until the ctest timeout.
    std::jthread producer([&queue](const std::stop_token& stop) {
        for (std::size_t i = 0; i < total; ++i) {
            while (!queue.try_push(std::size_t{i})) {
                if (stop.stop_requested()) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    std::size_t expected = 0;
    while (expected < total) {
        if (auto item = queue.try_pop()) {
            ASSERT_EQ(*item, expected);
            ++expected;
        }
    }
    EXPECT_EQ(expected, total);
}

// A non-trivial T at capacity 2: every operation sits on the full/empty
// boundary, and the consumer's destroy_at must happen before it publishes
// head_. With a trivially destructible T (above), TSan cannot see a slot
// being reused while its old element is still being destroyed.
TEST(SpscQueueTest, StressNonTrivialTypeAtCapacityTwo) {
    constexpr std::size_t total = 1'000'000;
    SpscQueue<std::unique_ptr<std::size_t>> queue{2};

    std::jthread producer([&queue](const std::stop_token& stop) {
        for (std::size_t i = 0; i < total; ++i) {
            auto item = std::make_unique<std::size_t>(i);
            while (!queue.try_push(std::move(item))) {
                if (stop.stop_requested()) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    std::size_t expected = 0;
    while (expected < total) {
        if (auto item = queue.try_pop()) {
            ASSERT_NE(*item, nullptr);
            ASSERT_EQ(**item, expected);
            ++expected;
        }
    }
    EXPECT_EQ(expected, total);
}

TEST(SpscQueueTest, DifferentialAgainstMutexQueue) {
    constexpr std::size_t capacity = 16;
    constexpr int operations = 100'000;
    SpscQueue<int> actual{capacity};
    MutexQueue<int> oracle{actual.capacity()};

    // Fixed seed: a failure must reproduce exactly.
    constexpr std::mt19937::result_type seed = 12345;
    std::mt19937 rng{seed};
    std::bernoulli_distribution push_or_pop{0.5};
    std::uniform_int_distribution<int> value_dist{0, 1'000'000};

    for (int op = 0; op < operations; ++op) {
        if (push_or_pop(rng)) {
            const int value = value_dist(rng);
            int actual_value = value;
            int oracle_value = value;
            const bool actual_ok = actual.try_push(std::move(actual_value));
            const bool oracle_ok = oracle.try_push(std::move(oracle_value));
            ASSERT_EQ(actual_ok, oracle_ok) << "op " << op;
        } else {
            auto actual_item = actual.try_pop();
            auto oracle_item = oracle.try_pop();
            ASSERT_EQ(actual_item.has_value(), oracle_item.has_value()) << "op " << op;
            if (actual_item.has_value()) {
                EXPECT_EQ(*actual_item, *oracle_item) << "op " << op;
            }
        }
    }
}

TEST(SpscQueueTest, LifetimeDestroysElementsStillQueuedOnDestruction) {
    // Single-threaded test: plain counters. `live` catches a leak and a
    // double destroy separately, which equal totals alone could hide.
    static int constructions = 0;
    static int destructions = 0;
    static int live = 0;

    struct Tracked {
        Tracked() {
            ++constructions;
            ++live;
        }
        Tracked(const Tracked&) = delete;
        Tracked& operator=(const Tracked&) = delete;
        Tracked(Tracked&&) noexcept {
            ++constructions;
            ++live;
        }
        Tracked& operator=(Tracked&&) = delete;
        ~Tracked() {
            ++destructions;
            --live;
            EXPECT_GE(live, 0) << "double destroy";
        }
    };

    {
        SpscQueue<Tracked> queue{4};
        ASSERT_TRUE(queue.try_push(Tracked{}));
        ASSERT_TRUE(queue.try_push(Tracked{}));
        ASSERT_TRUE(queue.try_push(Tracked{}));
        // One popped and dropped immediately, two left queued at destruction.
        auto popped = queue.try_pop();
        ASSERT_TRUE(popped.has_value());
    }

    EXPECT_EQ(live, 0) << "leak";
    EXPECT_EQ(constructions, destructions);
}

TEST(SpscQueueTest, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ((SpscQueue<int>{5}.capacity()), 8U);
}

TEST(SpscQueueTest, CapacityRoundingAtTheEdges) {
    EXPECT_EQ((SpscQueue<int>{0}.capacity()), 2U);
    EXPECT_EQ((SpscQueue<int>{1}.capacity()), 2U);
    EXPECT_EQ((SpscQueue<int>{2}.capacity()), 2U);
    EXPECT_EQ((SpscQueue<int>{3}.capacity()), 4U);
    EXPECT_EQ((SpscQueue<int>{64}.capacity()), 64U);
}

TEST(SpscQueueTest, TryPushOnFullQueueLeavesArgumentIntact) {
    SpscQueue<std::unique_ptr<int>> queue{2};
    ASSERT_TRUE(queue.try_push(std::make_unique<int>(1)));
    ASSERT_TRUE(queue.try_push(std::make_unique<int>(2)));

    auto extra = std::make_unique<int>(42);
    EXPECT_FALSE(queue.try_push(std::move(extra)));
    // NOLINTNEXTLINE(bugprone-use-after-move): try_push leaves it intact on failure
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(*extra, 42);
}

TEST(SpscQueueTest, FifoWrapAroundAcrossManyCyclesAtSmallCapacity) {
    SpscQueue<int> queue{2};
    ASSERT_EQ(queue.capacity(), 2U);

    // Push/pop past many multiples of capacity so the monotonic index wraps
    // (via masking) many times over, single-threaded so ordering is trivial
    // to check by hand.
    constexpr int cycles = 10'000;
    int next_push = 0;
    int next_pop = 0;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        ASSERT_TRUE(queue.try_push(int{next_push})) << "cycle " << cycle;
        ++next_push;
        ASSERT_TRUE(queue.try_push(int{next_push})) << "cycle " << cycle;
        ++next_push;
        // Queue is now full; a third push must fail without consuming state.
        EXPECT_FALSE(queue.try_push(int{-1}));

        auto first = queue.try_pop();
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(*first, next_pop++);
        auto second = queue.try_pop();
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(*second, next_pop++);

        EXPECT_FALSE(queue.try_pop().has_value());
    }
}

TEST(SpscQueueTest, MoveOnlyNonDefaultConstructibleType) {
    SpscQueue<MoveOnly> queue{4};
    ASSERT_TRUE(queue.try_push(MoveOnly{7}));
    ASSERT_TRUE(queue.try_push(MoveOnly{8}));

    auto first = queue.try_pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->value, 7);
    auto second = queue.try_pop();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->value, 8);
    EXPECT_FALSE(queue.try_pop().has_value());
}

}  // namespace
}  // namespace lockstep::concurrency
