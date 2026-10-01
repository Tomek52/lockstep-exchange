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
#include <stop_token>
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

    // Each producer checks the stop token in its retry loop. If a consumer
    // assertion below fails, the TEST body returns early and the jthreads'
    // destructors request_stop() and join(); without this check a producer
    // still spinning on a full queue (because the consumer stopped
    // consuming) would hang that join forever.
    std::vector<std::jthread> threads;
    threads.reserve(producers);
    for (std::uint64_t p = 0; p < producers; ++p) {
        threads.emplace_back([&queue, p](const std::stop_token& stop) {
            for (std::uint64_t i = 0; i < per_producer; ++i) {
                std::uint64_t value = (p << 32U) | i;
                while (!queue.try_push(std::move(value))) {
                    if (stop.stop_requested()) {
                        return;
                    }
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

TEST(MpscQueueTest, LifetimeDestroysWrappedLeftoversAfterCapacityCyclesThroughTwice) {
    // Same counting type as above, but driven past a full wrap of the ring
    // first: three push+pop cycles at capacity 4 advance enqueue_pos_ and
    // dequeue_pos_ to 3, reusing physical slots 0 and 1 along the way. The
    // three items left queued at destruction then occupy physical slots
    // {3, 0, 1}, not a contiguous [0, n) prefix, so this exercises the
    // destructor's masked indexing rather than just its loop bound.
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
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(queue.try_push(Tracked{}));
            auto popped = queue.try_pop();
            ASSERT_TRUE(popped.has_value());
        }
        // Three more left queued at destruction, wrapped into slots 3, 0, 1.
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(queue.try_push(Tracked{}));
        }
    }

    EXPECT_EQ(live, 0) << "leak";
    EXPECT_EQ(constructions, destructions);
}

TEST(MpscQueueTest, ProducerStalledBetweenClaimAndPublishTransientlyHidesLaterItems) {
    // Pins the property documented on MpscQueue's class comment: a producer
    // that is preempted between claiming its slot (the CAS) and publishing
    // it (the release store to sequence) makes the queue report "empty"
    // even though a later slot already holds a fully published item.
    struct BlockingElement {
        int tag;
        // Set (and notified) once this element's move constructor starts;
        // lets the main thread know producer A is inside try_push(),
        // past the CAS but before the publishing store. nullptr for
        // elements that should not block (e.g. producer B's).
        std::atomic<bool>* entered = nullptr;
        // The move constructor blocks here until this becomes true; lets
        // the test control exactly when producer A's push completes.
        std::atomic<bool>* release = nullptr;

        explicit BlockingElement(int t,
                                 std::atomic<bool>* entered_flag = nullptr,
                                 std::atomic<bool>* release_flag = nullptr)
            : tag{t}, entered{entered_flag}, release{release_flag} {}
        BlockingElement(const BlockingElement&) = delete;
        BlockingElement& operator=(const BlockingElement&) = delete;
        BlockingElement(BlockingElement&& other) noexcept
            : tag{other.tag}, entered{other.entered}, release{other.release} {
            if (entered != nullptr) {
                entered->store(true, std::memory_order_release);
                entered->notify_all();
            }
            if (release != nullptr) {
                release->wait(false, std::memory_order_acquire);
            }
        }
        BlockingElement& operator=(BlockingElement&&) = delete;
        ~BlockingElement() = default;
    };

    MpscQueue<BlockingElement> queue{4};

    std::atomic<bool> a_entered{false};
    std::atomic<bool> a_release{false};
    std::atomic<bool> a_push_ok{false};

    // Producer A: claims a slot, then blocks inside construct_at's move
    // constructor before publishing it. Push success is recorded for the
    // main thread to check after join rather than asserted on this thread
    // (gtest assertions are not meant to cross threads).
    std::jthread producer_a([&queue, &a_entered, &a_release, &a_push_ok] {
        BlockingElement element{1, &a_entered, &a_release};
        a_push_ok.store(queue.try_push(std::move(element)), std::memory_order_relaxed);
    });

    a_entered.wait(false, std::memory_order_acquire);
    // Happens-after the acquire wait above: producer A's CAS has already
    // claimed its slot and it is now blocked before publishing it.

    BlockingElement b{2};
    ASSERT_TRUE(queue.try_push(std::move(b)));  // claims the next slot, publishes immediately

    // B's item is fully published, but it sits behind A's claimed-but-not-
    // yet-published slot: try_pop() must report empty, not skip ahead to B.
    EXPECT_FALSE(queue.try_pop().has_value());

    a_release.store(true, std::memory_order_release);
    a_release.notify_all();
    producer_a.join();
    EXPECT_TRUE(a_push_ok.load(std::memory_order_relaxed));

    auto first = queue.try_pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->tag, 1);
    auto second = queue.try_pop();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->tag, 2);
    EXPECT_FALSE(queue.try_pop().has_value());
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

    // See the stop-token note in the stress test above: without it, a
    // consumer assertion failure here could leave a producer spinning on a
    // full queue forever, hanging the jthreads' join on scope exit.
    std::vector<std::jthread> threads;
    threads.reserve(producers);
    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&queue, p](const std::stop_token& stop) {
            for (std::size_t i = 0; i < per_producer; ++i) {
                auto item = std::make_unique<std::size_t>((p << 32U) | i);
                while (!queue.try_push(std::move(item))) {
                    if (stop.stop_requested()) {
                        return;
                    }
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
