// Contract tests every queue implementation must pass: MutexQueue, SpscQueue
// (task 005) and MpscQueue (task 006) are all registered in the type lists
// below; the multi-producer stress test also runs under the TSan preset.
#include <algorithm>
#include <cstddef>
#include <memory>
#include <ranges>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/concurrency/mpsc_queue.hpp"
#include "lockstep/concurrency/mutex_queue.hpp"
#include "lockstep/concurrency/queue_concepts.hpp"
#include "lockstep/concurrency/spsc_queue.hpp"

namespace lockstep::concurrency {
namespace {

// Concept conformance for every queue implementation.
static_assert(MultiProducerQueue<MutexQueue<int>>);
static_assert(MultiProducerQueue<MpscQueue<int>>);
static_assert(SingleProducerQueue<SpscQueue<int>>);
static_assert(!MultiProducerQueue<SpscQueue<int>>);

template <typename Q>
class QueueContract : public ::testing::Test {};

using AllQueues = ::testing::Types<MutexQueue<std::unique_ptr<int>>,
                                   SpscQueue<std::unique_ptr<int>>,
                                   MpscQueue<std::unique_ptr<int>>>;
TYPED_TEST_SUITE(QueueContract, AllQueues);

TYPED_TEST(QueueContract, PopFromEmptyReturnsNothing) {
    TypeParam queue{4};
    EXPECT_FALSE(queue.try_pop().has_value());
}

TYPED_TEST(QueueContract, PreservesFifoOrder) {
    TypeParam queue{8};
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(queue.try_push(std::make_unique<int>(i)));
    }
    for (int i = 0; i < 5; ++i) {
        auto item = queue.try_pop();
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(**item, i);
    }
}

TYPED_TEST(QueueContract, FullQueueRejectsWithoutConsumingTheValue) {
    TypeParam queue{2};
    const auto capacity = queue.capacity();
    for (std::size_t i = 0; i < capacity; ++i) {
        ASSERT_TRUE(queue.try_push(std::make_unique<int>(0)));
    }
    auto extra = std::make_unique<int>(42);
    EXPECT_FALSE(queue.try_push(std::move(extra)));
    ASSERT_NE(extra, nullptr)
        << "a failed push must not move from its argument";  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(*extra, 42);
}

template <typename Q>
class MultiProducerContract : public ::testing::Test {};

using MultiProducerQueues = ::testing::Types<MutexQueue<std::uint64_t>, MpscQueue<std::uint64_t>>;
TYPED_TEST_SUITE(MultiProducerContract, MultiProducerQueues);

TYPED_TEST(MultiProducerContract, EveryItemArrivesExactlyOnceAndPerProducerOrderHolds) {
    constexpr std::uint64_t producers = 4;
    constexpr std::uint64_t per_producer = 20'000;
    TypeParam queue{1024};

    // Each producer checks the stop token in its retry loop. If a consumer
    // assertion below fails, the TEST body returns early and the jthreads'
    // destructors request_stop() and join(); without this check a producer
    // still spinning on a full queue (because the consumer stopped
    // consuming) would hang that join forever.
    std::vector<std::jthread> threads;
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

}  // namespace
}  // namespace lockstep::concurrency
