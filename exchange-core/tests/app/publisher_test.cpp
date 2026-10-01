// Task 011: runtime subscriptions and the slow-consumer policy.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <span>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/app/subscription.hpp"

#include "app/test_support.hpp"

namespace lockstep::app {
namespace {

using namespace domain;

NewOrder buy(InstrumentId instrument) {
    return NewOrder{.trader = TraderId{1},
                    .client_order_id = ClientOrderId{1},
                    .instrument = instrument,
                    .side = Side::Buy,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = Price{100},
                    .quantity = Quantity{10}};
}

class PublisherSubscriptionTest : public ::testing::Test {
protected:
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    // One shard for both instruments: keeps event order across the two
    // commands in these tests unambiguous (ADR-0003 only guarantees order
    // per shard).
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}, {.id = InstrumentId{2}}},
                               .shard_count = 1},
                  journals.factory(), clock};

    void SetUp() override { engine.start(); }
};

TEST_F(PublisherSubscriptionTest, FilteredSubscriptionSeesOnlyThatInstrumentInOrder) {
    // private_events=true: this test is about the instrument filter, not the
    // event-category filter, so it also wants OrderAccepted (a private event
    // per SubscriptionFilter's doc) alongside the public BookLevelChanged.
    auto sub = engine.subscribe(
        SubscriptionFilter{.instruments = {InstrumentId{2}}, .private_events = true}, 64);

    NewOrder first = buy(InstrumentId{1});
    auto [c1, r1] = test::reply_future();
    ASSERT_TRUE(engine.submit(first, std::move(c1)).has_value());
    ASSERT_EQ(r1.wait_for(test::reply_timeout), std::future_status::ready);

    // A distinct client_order_id: the first order is still resting (GTC
    // limit, nothing to cross), and dedup is keyed on (trader,
    // client_order_id) across every book in the shard, not per instrument.
    NewOrder second = buy(InstrumentId{2});
    second.client_order_id = ClientOrderId{2};
    auto [c2, r2] = test::reply_future();
    ASSERT_TRUE(engine.submit(second, std::move(c2)).has_value());
    // By the time the ack for command 2 is visible, its events were already
    // flushed to every subscription (Publisher::poll_once flushes before
    // running the completion), so polling right after this is race-free.
    ASSERT_EQ(r2.wait_for(test::reply_timeout), std::future_status::ready);

    std::vector<PublishedEvent> buf(16);
    const std::size_t n = sub->poll(std::span{buf});
    // Non-crossing GTC limit on instrument 2 only: OrderAccepted then
    // BookLevelChanged as it rests - nothing from instrument 1's command.
    ASSERT_EQ(n, 2U);
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(buf[0].event));
    EXPECT_EQ(std::get<OrderAccepted>(buf[0].event).instrument, InstrumentId{2});
    ASSERT_TRUE(std::holds_alternative<BookLevelChanged>(buf[1].event));
    EXPECT_EQ(std::get<BookLevelChanged>(buf[1].event).instrument, InstrumentId{2});
    EXPECT_FALSE(sub->overflowed());
}

TEST_F(PublisherSubscriptionTest, SlowConsumerDoesNotBlockPublisherOrFastSubscription) {
    auto slow = engine.subscribe(SubscriptionFilter{}, 8);
    // Large enough that ~10,000 commands' worth of OrderAccepted/Trade/
    // BookLevelChanged events (at most a handful per command) cannot
    // overflow it - this subscription is the control for "the publisher
    // keeps delivering to a fast subscription" while `slow` is never polled.
    auto fast = engine.subscribe(SubscriptionFilter{}, 1U << 17U);

    std::atomic<bool> slow_ready{false};
    slow->on_ready([&slow_ready]() noexcept { slow_ready.store(true, std::memory_order_relaxed); });

    constexpr int total = 10'000;
    std::atomic<int> accepted{0};
    for (int i = 0; i < total; ++i) {
        NewOrder order = buy(InstrumentId{1});
        order.client_order_id = ClientOrderId{static_cast<std::uint64_t>(i)};
        order.side = (i % 2 == 0) ? Side::Buy : Side::Sell;  // crosses, generates Trade events too
        // Retry on a transiently full ingress (this loop submits far faster
        // than one shard thread drains) - the thing under test is the
        // publisher/subscription path, not ingress sizing.
        while (true) {
            auto submitted = engine.submit(order, [&accepted](const CommandReply&) noexcept {
                accepted.fetch_add(1, std::memory_order_relaxed);
            });
            if (submitted.has_value()) {
                break;
            }
            ASSERT_EQ(submitted.error(), SubmitError::Overloaded);
            std::this_thread::yield();
        }
    }

    // Bounded wait, not a hang: if a full, never-polled `slow` ever blocked
    // the publisher (and transitively the shard, ADR-0003), accepted would
    // stall below total and this loop would run out the clock instead of
    // completing promptly.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (accepted.load(std::memory_order_relaxed) < total &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(accepted.load(std::memory_order_relaxed), total)
        << "a full, never-polled subscription stalled the publisher";

    engine.stop();

    EXPECT_TRUE(slow->overflowed());
    EXPECT_TRUE(slow_ready.load(std::memory_order_relaxed));

    std::vector<PublishedEvent> buf(256);
    std::size_t drained = 0;
    while (fast->poll(std::span{buf}) > 0) {
        ++drained;
    }
    EXPECT_GT(drained, 0U);
    EXPECT_FALSE(fast->overflowed());
}

TEST_F(PublisherSubscriptionTest, SubscriptionMidStreamSeesOnlyLaterEventsAtomically) {
    // Before anyone subscribes.
    auto [c1, r1] = test::reply_future();
    ASSERT_TRUE(engine.submit(buy(InstrumentId{1}), std::move(c1)).has_value());
    ASSERT_EQ(r1.wait_for(test::reply_timeout), std::future_status::ready);

    // subscribe() only returns once the registration is visible to the
    // publisher's control queue (see Publisher::subscribe); every command
    // submitted from here on is guaranteed to come after that in program
    // order, so Publisher::poll_once's apply_control()-before-any-source
    // ordering is enough to make this race-free without a settling sleep.
    // private_events=true: see FilteredSubscriptionSeesOnlyThatInstrumentInOrder.
    auto sub = engine.subscribe(SubscriptionFilter{.instruments = {}, .private_events = true}, 64);

    // Distinct client_order_id: the first order is still resting and dedup
    // is keyed on (trader, client_order_id) across every book in the shard.
    NewOrder second = buy(InstrumentId{2});
    second.client_order_id = ClientOrderId{2};
    auto [c2, r2] = test::reply_future();
    ASSERT_TRUE(engine.submit(second, std::move(c2)).has_value());
    ASSERT_EQ(r2.wait_for(test::reply_timeout), std::future_status::ready);

    std::vector<PublishedEvent> buf(16);
    const std::size_t n = sub->poll(std::span{buf});
    // Only the second command's events (OrderAccepted + BookLevelChanged);
    // the first command's pair, produced before registration, must not
    // appear even partially.
    ASSERT_EQ(n, 2U);
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(buf[0].event));
    EXPECT_EQ(std::get<OrderAccepted>(buf[0].event).instrument, InstrumentId{2});
    ASSERT_TRUE(std::holds_alternative<BookLevelChanged>(buf[1].event));
    EXPECT_EQ(std::get<BookLevelChanged>(buf[1].event).instrument, InstrumentId{2});
}

TEST_F(PublisherSubscriptionTest, CancelStopsDelivery) {
    auto sub = engine.subscribe(SubscriptionFilter{}, 64);
    sub->cancel();
    sub->cancel();  // idempotent

    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(engine.submit(buy(InstrumentId{1}), std::move(completion)).has_value());
    ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    engine.stop();

    std::vector<PublishedEvent> buf(16);
    EXPECT_EQ(sub->poll(std::span{buf}), 0U);
    EXPECT_FALSE(sub->overflowed());
}

TEST_F(PublisherSubscriptionTest, ConcurrentSubscribeAndCancelFromFourThreadsIsRaceFree) {
    std::atomic<bool> stop_workers{false};
    std::vector<std::jthread> workers;
    workers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([this, &stop_workers] {
            while (!stop_workers.load(std::memory_order_relaxed)) {
                auto sub =
                    engine.subscribe(SubscriptionFilter{.instruments = {InstrumentId{1}}}, 4);
                std::vector<PublishedEvent> buf(4);
                (void)sub->poll(std::span{buf});
                sub->cancel();
            }
        });
    }

    constexpr int total = 300;
    for (int i = 0; i < total; ++i) {
        auto [completion, reply] = test::reply_future();
        const InstrumentId instrument = (i % 2) == 0 ? InstrumentId{1} : InstrumentId{2};
        ASSERT_TRUE(engine.submit(buy(instrument), std::move(completion)).has_value());
        ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    }

    stop_workers.store(true, std::memory_order_relaxed);
    workers.clear();  // joins every worker
    engine.stop();
}

}  // namespace
}  // namespace lockstep::app
