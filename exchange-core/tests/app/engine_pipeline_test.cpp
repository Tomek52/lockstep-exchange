// Walking-skeleton test of the threaded pipeline:
// submit -> ingress -> shard (journal, apply) -> egress -> publisher -> completion.
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <variant>

#include <gtest/gtest.h>

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

class EnginePipelineTest : public ::testing::Test {
protected:
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    test::RecordingSubscriber subscriber;
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}, {.id = InstrumentId{2}}},
                               .shard_count = 2},
                  journals.factory(), clock};

    void SetUp() override {
        engine.add_subscriber(subscriber);
        engine.start();
    }
};

TEST_F(EnginePipelineTest, AcceptedOrderIsJournaledAppliedPublishedAndAcked) {
    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(engine.submit(buy(InstrumentId{2}), std::move(completion)).has_value());

    ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    const CommandReply ack = reply.get();
    EXPECT_EQ(ack.shard, ShardId{1});  // instrument 2 -> shard 1 (round robin)
    EXPECT_EQ(ack.sequence, SequenceNumber{1});
    EXPECT_EQ(ack.timestamp, Timestamp{1'000});
    ASSERT_TRUE(ack.result.has_value());

    engine.stop();
    const auto journaled = journals.of(ShardId{1}).committed();
    ASSERT_EQ(journaled.size(), 1U);
    EXPECT_EQ(std::get<NewOrder>(journaled[0].command), buy(InstrumentId{2}));

    // Non-crossing GTC limit: OrderAccepted, then BookLevelChanged as it rests
    // (task 002 matching).
    ASSERT_EQ(subscriber.events().size(), 2U);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(subscriber.events()[0].event));
    EXPECT_TRUE(std::holds_alternative<BookLevelChanged>(subscriber.events()[1].event));
}

TEST_F(EnginePipelineTest, DomainRejectionIsJournaledAndReturnedAsError) {
    NewOrder bad = buy(InstrumentId{1});
    bad.quantity = Quantity{0};
    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(engine.submit(bad, std::move(completion)).has_value());

    ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    EXPECT_EQ(reply.get().result.error(), RejectReason::InvalidQuantity);
    engine.stop();
    // Rejected commands are inputs too: replay must see them to reproduce ids.
    EXPECT_EQ(journals.of(ShardId{0}).committed().size(), 1U);
}

TEST_F(EnginePipelineTest, UnroutableCommandsFailSynchronously) {
    EXPECT_EQ(engine.submit(buy(InstrumentId{42}), {}).error(), SubmitError::UnknownInstrument);
    EXPECT_EQ(engine.submit(KillSwitch{}, {}).error(), SubmitError::NotRoutable);
}

TEST_F(EnginePipelineTest, BroadcastReachesEveryShard) {
    ASSERT_TRUE(engine.broadcast(BlockTrader{RiskCommandId{5}, TraderId{9}}).has_value());
    engine.stop();

    int applied = 0;
    for (const PublishedEvent& published : subscriber.events()) {
        if (const auto* ack = std::get_if<RiskCommandApplied>(&published.event)) {
            EXPECT_EQ(ack->command_id, RiskCommandId{5});
            ++applied;
        }
    }
    EXPECT_EQ(applied, 2);
    EXPECT_EQ(journals.of(ShardId{0}).committed().size(), 1U);
    EXPECT_EQ(journals.of(ShardId{1}).committed().size(), 1U);
}

TEST_F(EnginePipelineTest, SubmitAfterStopIsRefused) {
    engine.stop();
    EXPECT_EQ(engine.submit(buy(InstrumentId{1}), {}).error(), SubmitError::ShuttingDown);
}

// task 007: idle/parking strategy and runtime statistics.

TEST_F(EnginePipelineTest, StatsReflectCommandsBatchesAndMaxBatch) {
    for (int i = 0; i < 5; ++i) {
        auto [completion, reply] = test::reply_future();
        ASSERT_TRUE(engine.submit(buy(InstrumentId{2}), std::move(completion)).has_value());
        // Waiting for each reply before the next submit means the shard has
        // always fully drained and gone back to polling an empty ingress
        // before the next command can arrive, so each lands in its own
        // single-command batch - batches and max_batch are exact, not just
        // bounded.
        ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    }
    engine.stop();

    const std::vector<ShardStats> stats = engine.shard_stats();
    ASSERT_EQ(stats.size(), 2U);

    // Shard 0 (instrument 1) never received a command in this test.
    EXPECT_EQ(stats[0].commands, 0U);
    EXPECT_EQ(stats[0].batches, 0U);
    EXPECT_EQ(stats[0].max_batch, 0U);

    // Shard 1 (instrument 2) processed the 5 submitted commands in 5
    // separate single-command batches (see the comment above).
    EXPECT_EQ(stats[1].commands, 5U);
    EXPECT_EQ(stats[1].batches, 5U);
    EXPECT_EQ(stats[1].max_batch, 1U);
}

TEST_F(EnginePipelineTest, IdleShardParksRatherThanPolling) {
    // Long enough for the spin (256 iters) and yield (64 iters) phases to
    // exhaust and reach the park phase, which takes well under a
    // millisecond. ParkingIdle publishes parks_stat_ the moment it enters
    // Doorbell::wait() - before it blocks, not after it returns - so a
    // shard that has reached the park phase already reports >=1 here, even
    // though it may then stay blocked inside that single wait() call for
    // the rest of the test (the old publish-after-return design could not
    // tell a thread genuinely parked like that apart from one that never
    // reached the park phase at all: both read 0).
    //
    // Deliberately not a delta between the two snapshots below (reviewed in
    // task 007's follow-up): if the shard parks once and then stays blocked
    // in that single wait() call for the entire 200ms window - the *best*
    // possible outcome - the delta between an after_settle and an after_idle
    // reading is legitimately 0, which a `delta >= 1` assertion would wrongly
    // fail against a correct implementation. Using an absolute floor right
    // after settling (it must have reached the park phase by then) and an
    // absolute ceiling after the full idle window (it must not be spinning)
    // avoids that false failure while still catching both bugs.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const std::vector<ShardStats> after_settle = engine.shard_stats();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const std::vector<ShardStats> after_idle = engine.shard_stats();
    engine.stop();

    ASSERT_EQ(after_settle.size(), after_idle.size());
    for (std::size_t i = 0; i < after_settle.size(); ++i) {
        EXPECT_GE(after_settle[i].parks, 1U);
        // Staying blocked in one wait() call for the whole 200ms (parks
        // unchanged since after_settle) is the *best* outcome, not a
        // failure - this only bounds the other end: a thread that is
        // spinning instead of really parking would rack up thousands of
        // wait() calls, not a handful.
        EXPECT_LE(after_idle[i].parks, 5U);
    }
}

TEST_F(EnginePipelineTest, StopReturnsPromptlyOnIdleEngine) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});  // let it actually park first
    const auto start = std::chrono::steady_clock::now();
    engine.stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds{100});
}

// task 007 follow-up (code review): a batch whose output does not fit in a
// full, parked publisher's egress queue must not livelock the shard. Both
// tests below heap-allocate Engine and only call stop() on the success path:
// stopping a livelocked shard would itself hang in Engine::stop()'s join(),
// since that shard thread never reaches its stop_requested() check while
// stuck retrying a full egress queue - a bounded reply.wait_for() still
// reports the failure below instead of hanging the test binary, and on
// failure the Engine (and its still-spinning thread) is deliberately leaked
// rather than destroyed.

TEST(ShardPublisherBackpressure, ReplyArrivesWhenPublisherIsParkedAndEgressIsTiny) {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    test::RecordingSubscriber subscriber;
    // A single resting GTC limit order stages OrderAccepted + BookLevelChanged
    // + its ReplyTask = 3 items, already more than this capacity.
    auto engine = std::make_unique<Engine>(
        EngineConfig{
            .instruments = {{.id = InstrumentId{1}}}, .shard_count = 1, .egress_capacity = 2},
        journals.factory(), clock);
    engine->add_subscriber(subscriber);
    engine->start();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});  // let the publisher park

    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(engine->submit(buy(InstrumentId{1}), std::move(completion)).has_value());
    const bool got_reply = reply.wait_for(test::reply_timeout) == std::future_status::ready;
    EXPECT_TRUE(got_reply) << "shard livelocked retrying a full egress queue "
                              "without waking the parked publisher";
    if (got_reply) {
        engine->stop();
    } else {
        (void)engine.release();  // see the comment above this test
    }
}

TEST(ShardPublisherBackpressure, BatchExceedingEgressCapacityStillDrains) {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    test::RecordingSubscriber subscriber;
    auto engine = std::make_unique<Engine>(
        EngineConfig{
            .instruments = {{.id = InstrumentId{1}}}, .shard_count = 1, .egress_capacity = 4},
        journals.factory(), clock);
    engine->add_subscriber(subscriber);
    engine->start();

    // Rest 5 sell orders one at a time (each ack'd before the next, so none
    // of them alone exceeds capacity=4).
    for (std::uint32_t i = 0; i < 5; ++i) {
        auto [completion, reply] = test::reply_future();
        NewOrder sell{.trader = TraderId{100 + i},
                      .client_order_id = ClientOrderId{1},
                      .instrument = InstrumentId{1},
                      .side = Side::Sell,
                      .type = OrderType::Limit,
                      .time_in_force = TimeInForce::Gtc,
                      .price = Price{100},
                      .quantity = Quantity{1}};
        ASSERT_TRUE(engine->submit(sell, std::move(completion)).has_value());
        ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});  // let the publisher park

    // One aggressive buy sweeps all 5 resting sells in a single command: the
    // Trade/BookLevelChanged/ReplyTask output from processing it (well over
    // 4 items) is staged and released as one batch, entirely after the
    // publisher parked.
    auto [completion, reply] = test::reply_future();
    NewOrder sweep{.trader = TraderId{1},
                   .client_order_id = ClientOrderId{2},
                   .instrument = InstrumentId{1},
                   .side = Side::Buy,
                   .type = OrderType::Limit,
                   .time_in_force = TimeInForce::Gtc,
                   .price = Price{100},
                   .quantity = Quantity{5}};
    ASSERT_TRUE(engine->submit(sweep, std::move(completion)).has_value());
    const bool got_reply = reply.wait_for(test::reply_timeout) == std::future_status::ready;
    EXPECT_TRUE(got_reply) << "shard livelocked retrying a full egress queue "
                              "without waking the parked publisher";
    if (got_reply) {
        engine->stop();
    } else {
        (void)engine.release();  // see the comment above the previous test
    }
}

}  // namespace
}  // namespace lockstep::app
