// Tests specific to MpscQueue: stress (task 006 acceptance criterion 2),
// full-queue behaviour, differential against MutexQueue, lifetime, and
// wrap-around / slot-reuse under contention.
#include "lockstep/concurrency/mpsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/concurrency/mutex_queue.hpp"

namespace lockstep::concurrency {
namespace {

TEST(MpscQueueTest, StressEightProducersFiveHundredThousandEachExactlyOnceAndPerProducerFifo) {
    constexpr std::uint64_t producers = 8;
    constexpr std::uint64_t per_producer = 500'000;
    MpscQueue<std::uint64_t> queue{1024};

    std::vector<std::jthread> threads;
    threads.reserve(producers);
    for (std::uint64_t p = 0; p < producers; ++p) {
        threads.emplace_back([&queue, p] {
            for (std::uint64_t i = 0; i < per_producer; ++i) {
                std::uint64_t value = (p << 32U) | i;
                while (!queue.try_push(std::move(value))) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::uint64_t> next_expected(producers, 0);
    for (std::uint64_t received = 0; received < producers * per_producer;) {
        if (auto item = queue.try_pop()) {
            const auto producer = *item >> 32U;
            const auto sequence = *item & 0xFFFF'FFFFU;
            ASSERT_LT(producer, producers);
            ASSERT_EQ(sequence, next_expected[producer]) << "per-producer FIFO violated";
            ++next_expected[producer];
            ++received;
        }
    }
    EXPECT_TRUE(std::ranges::all_of(next_expected, [](auto n) { return n == per_producer; }));
}

TEST(MpscQueueTest, FullQueueRejectsWithoutConsumingTheValue) {
    MpscQueue<std::unique_ptr<int>> queue{4};
    const auto capacity = queue.capacity();
    ASSERT_EQ(capacity, 4U);
    for (std::size_t i = 0; i < capacity; ++i) {
        ASSERT_TRUE(queue.try_push(std::make_unique<int>(static_cast<int>(i))));
    }

    auto extra = std::make_unique<int>(42);
    EXPECT_FALSE(queue.try_push(std::move(extra)));
    // NOLINTNEXTLINE(bugprone-use-after-move): try_push leaves it intact on failure
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(*extra, 42);
}

TEST(MpscQueueTest, DifferentialAgainstMutexQueue) {
    constexpr std::size_t capacity = 16;
    constexpr int operations = 100'000;
    MpscQueue<int> actual{capacity};
    MutexQueue<int> oracle{actual.capacity()};

    // Fixed seed: a failure must reproduce exactly.
    constexpr std::mt19937::result_type seed = 67890;
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

TEST(MpscQueueTest, LifetimeDestroysElementsStillQueuedOnDestruction) {
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
        MpscQueue<Tracked> queue{4};
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

TEST(MpscQueueTest, CapacityRoundsUpToPowerOfTwoWithMinimumTwo) {
    EXPECT_EQ((MpscQueue<int>{0}.capacity()), 2U);
    EXPECT_EQ((MpscQueue<int>{1}.capacity()), 2U);
    EXPECT_EQ((MpscQueue<int>{2}.capacity()), 2U);
    EXPECT_EQ((MpscQueue<int>{3}.capacity()), 4U);
    EXPECT_EQ((MpscQueue<int>{5}.capacity()), 8U);
    EXPECT_EQ((MpscQueue<int>{64}.capacity()), 64U);
}

TEST(MpscQueueTest, FifoWrapAroundAcrossManyCyclesSingleThreaded) {
    MpscQueue<int> queue{2};
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
        // Queue is now full: a third push must fail without consuming state.
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

// A non-trivial, non-default-constructible T at capacity 2 with several
// producers: every slot cycles between free and full on every lap, so a
// bug in the sequence arithmetic (e.g. reusing a slot before the consumer's
// destroy_at happened before it) is likely to be caught by TSan here even
// though the single-threaded wrap-around test above cannot see it.
TEST(MpscQueueTest, StressNonTrivialTypeAtCapacityTwoUnderContention) {
    constexpr std::size_t producers = 4;
    constexpr std::size_t per_producer = 200'000;
    MpscQueue<std::unique_ptr<std::size_t>> queue{2};

    std::vector<std::jthread> threads;
    threads.reserve(producers);
    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&queue, p] {
            for (std::size_t i = 0; i < per_producer; ++i) {
                auto item = std::make_unique<std::size_t>((p << 32U) | i);
                while (!queue.try_push(std::move(item))) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::size_t> next_expected(producers, 0);
    for (std::size_t received = 0; received < producers * per_producer;) {
        if (auto item = queue.try_pop()) {
            ASSERT_NE(*item, nullptr);
            const auto producer = **item >> 32U;
            const auto sequence = **item & 0xFFFF'FFFFU;
            ASSERT_LT(producer, producers);
            ASSERT_EQ(sequence, next_expected[producer]) << "per-producer FIFO violated";
            ++next_expected[producer];
            ++received;
        }
    }
    EXPECT_TRUE(std::ranges::all_of(next_expected, [](auto n) { return n == per_producer; }));
}

}  // namespace
}  // namespace lockstep::concurrency
