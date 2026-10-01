// Walking-skeleton test of the threaded pipeline:
// submit -> ingress -> shard (journal, apply) -> egress -> publisher -> completion.
#include <chrono>
#include <future>
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
        ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    }
    engine.stop();

    const std::vector<ShardStats> stats = engine.shard_stats();
    ASSERT_EQ(stats.size(), 2U);

    // Shard 0 (instrument 1) never received a command in this test.
    EXPECT_EQ(stats[0].commands, 0U);
    EXPECT_EQ(stats[0].batches, 0U);
    EXPECT_EQ(stats[0].max_batch, 0U);

    // Shard 1 (instrument 2) processed exactly the 5 submitted commands,
    // each one waited for individually, so in at least one and at most
    // five batches (the shard may or may not coalesce a next command that
    // arrives before it next polls).
    EXPECT_EQ(stats[1].commands, 5U);
    EXPECT_GE(stats[1].batches, 1U);
    EXPECT_LE(stats[1].batches, 5U);
    EXPECT_GE(stats[1].max_batch, 1U);
    EXPECT_LE(stats[1].max_batch, 5U);
    // batches * max_batch can't under-cover every command.
    EXPECT_GE(stats[1].batches * stats[1].max_batch, stats[1].commands);
}

TEST_F(EnginePipelineTest, IdleShardParksRatherThanPolling) {
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const std::vector<ShardStats> stats = engine.shard_stats();
    engine.stop();

    // A thread that is truly parked wakes only a handful of times in 200ms
    // of silence (startup, maybe a spurious wake); one that is still
    // polling in a tight loop would rack up thousands.
    for (const ShardStats& shard_stats : stats) {
        EXPECT_LE(shard_stats.parks, 5U);
    }
}

TEST_F(EnginePipelineTest, StopReturnsPromptlyOnIdleEngine) {
    const auto start = std::chrono::steady_clock::now();
    engine.stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds{100});
}

}  // namespace
}  // namespace lockstep::app
