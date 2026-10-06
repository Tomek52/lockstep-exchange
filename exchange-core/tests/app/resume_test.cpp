// Engine/ShardRuntime resume-from-journal (ADR-0020, task 010): a
// ResumeFactory replays recovered commands into a shard's engine before any
// thread starts, continuing order ids, books and sequence numbers, without
// publishing the replayed outputs anywhere.
#include <array>
#include <future>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "app/test_support.hpp"

namespace lockstep::app {
namespace {

using namespace domain;

NewOrder buy(TraderId trader, ClientOrderId client_order_id, InstrumentId instrument, Price price) {
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
    const RestingOrder* resumed = book->front(Side::Buy);
    ASSERT_NE(resumed, nullptr);
    const OrderId resumed_order_id = resumed->id;

    engine.add_subscriber(subscriber);
    engine.start();

    // A new order on the same shard must get the next order id and the next
    // sequence number after the resumed one, not restart from zero/one.
    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(engine
                    .submit(buy(TraderId{2}, ClientOrderId{2}, InstrumentId{1}, Price{40}),
                            std::move(completion))
                    .has_value());
    ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    const CommandReply ack = reply.get();
    engine.stop();

    ASSERT_TRUE(ack.result.has_value());
    EXPECT_EQ(ack.sequence, SequenceNumber{2})
        << "sequence numbering must resume after the replayed record, not restart at 1";
    // Order ids are a per-shard counter (ShardEngine::next_order_id): the
    // resumed order must have consumed the first one, so this new order's
    // id must be strictly greater.
    EXPECT_GT(ack.result->order_id.value(), resumed_order_id.value());

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

// Task 010 review F2: a crash can leave the journal's last known risk-link
// state as "connected" (the link was up when the process died). Resuming
// that state naively and then accepting traffic would let FailClosed treat
// the link as still up with nothing actually connected. ADR-0020's fix is
// that the composition root journals RiskLinkStatus{connected=false} as
// this run's first live command, overriding the stale resumed state before
// anything can submit an order; this test exercises exactly that sequence
// at the Engine level, passing it to start() the same way main.cpp does
// (task 010 review auditor F-2).
TEST(Resume, RestartWithFailClosedRejectsOrdersUntilTheLinkReconnects) {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;

    // Simulates a journal whose last record was a stale "connected" - as if
    // the previous run's risk client had reconnected before the crash.
    ResumeFactory resume = [](ShardId shard) -> std::vector<SequencedCommand> {
        if (shard != ShardId{0}) {
            return {};
        }
        return {
            SequencedCommand{SequenceNumber{1}, Timestamp{900}, RiskLinkStatus{.connected = true}}};
    };
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}},
                               .shard_count = 1,
                               .risk_link_policy = RiskLinkPolicy::FailClosed},
                  journals.factory(), clock, std::move(resume)};
    const std::array<Command, 1> startup_commands{RiskLinkStatus{.connected = false}};
    engine.start(startup_commands);

    auto [completion, reply] = test::reply_future();
    ASSERT_TRUE(engine
                    .submit(buy(TraderId{1}, ClientOrderId{1}, InstrumentId{1}, Price{50}),
                            std::move(completion))
                    .has_value());
    ASSERT_EQ(reply.wait_for(test::reply_timeout), std::future_status::ready);
    const CommandReply ack = reply.get();
    engine.stop();

    ASSERT_FALSE(ack.result.has_value());
    EXPECT_EQ(ack.result.error(), RejectReason::RiskUnavailable);
}

// Task 010 review auditor F-2: start()'s startup_commands must be the first
// thing every shard's thread ever pops, even when submitters race to get an
// order in the instant start() returns - not merely "usually first" because
// nothing else had submitted yet. Exercised on a resumed journal (so every
// shard already has N1 >= 1 resumed records, and the race is against the
// *next* sequence number, not against an empty shard) and run under tsan.
// One round catches a reordering regression only about a third of the time
// (auditor measurement, 4 cores), so the test repeats it with a fresh
// engine to make a regression close to certain to show up in a single run.
void expect_startup_command_first_despite_concurrent_submitters() {
    constexpr std::uint32_t shard_count = 4;
    constexpr int submitter_count = 16;
    constexpr std::uint32_t submits_per_thread = 20;

    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;

    // Every shard resumes exactly one record, so N1 == 1 everywhere and the
    // startup command must land at sequence 2.
    ResumeFactory resume = [](ShardId /*shard*/) -> std::vector<SequencedCommand> {
        return {
            SequencedCommand{SequenceNumber{1}, Timestamp{500}, RiskLinkStatus{.connected = true}}};
    };

    std::vector<InstrumentSpec> instruments;
    for (std::uint32_t i = 0; i < shard_count; ++i) {
        instruments.push_back({.id = InstrumentId{i + 1}});
    }
    Engine engine{EngineConfig{.instruments = instruments, .shard_count = shard_count},
                  journals.factory(), clock, std::move(resume)};

    // Spawned and already spinning *before* start() is even called, so the
    // only latency between "accepting_ becomes true" and "a submitter's
    // try_push lands" is a spin-loop reload - thread creation (which would
    // otherwise dwarf the whole race window) happens entirely beforehand.
    // Each thread retries submit() until it stops seeing ShuttingDown (the
    // only error submit() can give before start(), per its precondition).
    std::vector<std::thread> submitters;
    submitters.reserve(submitter_count);
    for (int t = 0; t < submitter_count; ++t) {
        const auto trader = static_cast<std::uint32_t>(t);
        submitters.emplace_back([&engine, trader] {
            for (std::uint32_t i = 0; i < submits_per_thread; ++i) {
                const InstrumentId instrument{(i % shard_count) + 1};
                // Fire-and-forget: only the race to get *into* the ingress
                // queue matters here, not the reply.
                while (!engine
                            .submit(buy(TraderId{trader}, ClientOrderId{i}, instrument, Price{10}),
                                    [](const CommandReply&) noexcept {})
                            .has_value()) {
                    // Spin: before start(), submit() fails with
                    // ShuttingDown every time (accepting_ starts false).
                }
            }
        });
    }
    const std::array<Command, 1> startup_commands{RiskLinkStatus{.connected = false}};
    engine.start(startup_commands);
    for (auto& thread : submitters) {
        thread.join();
    }
    engine.stop();

    for (std::uint32_t s = 0; s < shard_count; ++s) {
        const auto committed = journals.of(ShardId{s}).committed();
        ASSERT_FALSE(committed.empty()) << "shard " << s;
        EXPECT_EQ(committed.front().sequence, SequenceNumber{2}) << "shard " << s;
        const auto* status = std::get_if<RiskLinkStatus>(&committed.front().command);
        ASSERT_NE(status, nullptr) << "shard " << s;
        EXPECT_FALSE(status->connected) << "shard " << s;
    }
}

TEST(Resume, StartupCommandIsFirstRecordDespiteConcurrentSubmitters) {
    constexpr int rounds = 10;
    for (int round = 0; round < rounds; ++round) {
        SCOPED_TRACE(testing::Message() << "round " << round);
        expect_startup_command_first_despite_concurrent_submitters();
        if (HasFailure()) {
            return;
        }
    }
}

// Task 010 review auditor F-B: broadcast() is refused before start(), like
// submit(), so nothing broadcast early (e.g. a risk client started too soon)
// can sit in an ingress ahead of the startup commands.
TEST(Resume, BroadcastBeforeStartIsRefusedAndStartupCommandStaysFirst) {
    ManualClock clock{1'000, 10};
    test::MemoryJournals journals;
    Engine engine{EngineConfig{.instruments = {{.id = InstrumentId{1}}}, .shard_count = 1},
                  journals.factory(), clock};

    const auto early = engine.broadcast(RiskLinkStatus{.connected = true});
    ASSERT_FALSE(early.has_value());
    EXPECT_EQ(early.error(), SubmitError::ShuttingDown);

    const std::array<Command, 1> startup_commands{RiskLinkStatus{.connected = false}};
    engine.start(startup_commands);
    engine.stop();

    const auto committed = journals.of(ShardId{0}).committed();
    ASSERT_EQ(committed.size(), 1U);
    const auto* status = std::get_if<RiskLinkStatus>(&committed.front().command);
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(status->connected);
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
