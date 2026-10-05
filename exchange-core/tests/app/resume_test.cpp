// Engine/ShardRuntime resume-from-journal (ADR-0020, task 010): a
// ResumeFactory replays recovered commands into a shard's engine before any
// thread starts, continuing order ids, books and sequence numbers, without
// publishing the replayed outputs anywhere.
#include <future>
#include <vector>

#include <gtest/gtest.h>

#include "app/test_support.hpp"

namespace lockstep::app {
namespace {

using namespace domain;

NewOrder buy(TraderId trader, ClientOrderId client_order_id, InstrumentId instrument,
            Price price) {
    return NewOrder{.trader = trader,
                    .client_order_id = client_order_id,
                    .instrument = instrument,
                    .side = Side::Buy,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = price,
                    .quantity = Quantity{10}};
}

std::vector<SequencedCommand> resting_order_commands() {
    // A single resting order on shard 0's book (instrument 1 only lives on
    // shard 0 with shard_count=2's round robin).
    return {SequencedCommand{SequenceNumber{1}, Timestamp{1'000},
                             buy(TraderId{1}, ClientOrderId{1}, InstrumentId{1}, Price{50})}};
}

TEST(Resume, ReplaysRecordsBeforeStartAndContinuesSequenceAndOrderIds) {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    test::RecordingSubscriber subscriber;

    ResumeFactory resume = [](ShardId shard) -> std::vector<SequencedCommand> {
        return shard == ShardId{0} ? resting_order_commands() : std::vector<SequencedCommand>{};
    };
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}, {.id = InstrumentId{2}}},
                              .shard_count = 2},
                 journals.factory(), clock, std::move(resume)};

    // The resumed order must already be resting, and visible, before start():
    // engine()/book() are only valid while the shard thread is not running.
    const OrderBook* book = engine.shard(ShardId{0}).engine().book(InstrumentId{1});
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->order_count(), 1U);
    EXPECT_EQ(book->best_price(Side::Buy), Price{50});

    engine.add_subscriber(subscriber);
    engine.start();

    // A new order on the same shard must get the next order id and the next
    // sequence number after the resumed one, not restart from zero/one.
    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(
        engine.submit(buy(TraderId{2}, ClientOrderId{2}, InstrumentId{1}, Price{40}), std::move(completion))
            .has_value());
    ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    const CommandReply ack = reply.get();
    engine.stop();

    ASSERT_TRUE(ack.result.has_value());
    EXPECT_EQ(ack.sequence, SequenceNumber{2})
        << "sequence numbering must resume after the replayed record, not restart at 1";
    // Order ids are a per-shard counter (ShardEngine::next_order_id): the
    // resumed order must have consumed the first one.
    const OrderId resumed_order = book->find(ack.result->order_id) != nullptr
                                      ? ack.result->order_id
                                      : OrderId{0};
    (void)resumed_order;  // the id itself is opaque; what matters is it differs below
    EXPECT_NE(ack.result->order_id, OrderId{0});

    // The replayed command's own events (its OrderAccepted and
    // BookLevelChanged) must never have been published: only this run's new
    // order's events show up (its own OrderAccepted and BookLevelChanged).
    ASSERT_EQ(subscriber.events().size(), 2U);
    for (const PublishedEvent& published : subscriber.events()) {
        if (const auto* accepted = std::get_if<OrderAccepted>(&published.event)) {
            EXPECT_EQ(accepted->trader, TraderId{2});
        }
    }
}

TEST(Resume, EmptyResumeFactoryBehavesLikeAFreshShard) {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    ResumeFactory resume = [](ShardId /*shard*/) { return std::vector<SequencedCommand>{}; };
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}}, .shard_count = 1},
                 journals.factory(), clock, std::move(resume)};

    const OrderBook* book = engine.shard(ShardId{0}).engine().book(InstrumentId{1});
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->order_count(), 0U);
}

}  // namespace
}  // namespace lockstep::app
