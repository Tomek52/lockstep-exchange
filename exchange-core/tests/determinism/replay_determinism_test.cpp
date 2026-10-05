// Deterministic replay (ADR-0004): whatever interleaving the live, multi-threaded
// run happened to take, replaying each shard's journal into a fresh ShardEngine
// must reproduce that shard's events, replies and book state exactly.
//
// The generator (generator.hpp, acceptance criterion 1) covers matching
// (crossing limits, market, IOC), cancels/modifies against real accepted
// ids, occasional risk broadcasts, and occasional invalid/duplicate
// commands - every one of those is journaled, including rejections, and
// replay must reproduce it identically.
#include <cstdint>
#include <map>
#include <random>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/app/replay.hpp"

#include "app/test_support.hpp"
#include "generator.hpp"

namespace lockstep {
namespace {

using namespace domain;

constexpr std::size_t shard_count = 2;
constexpr int commands_per_producer = 2'000;

TEST(ReplayDeterminism, JournalReplayReproducesLiveOutputPerShard) {
    app::ManualClock clock;
    test::MemoryJournals journals;
    test::RecordingSubscriber subscriber;
    std::vector<app::CommandReply> live_replies;  // written only on the publisher thread

    // The engine owns the journals, so it must outlive the comparison below.
    app::Engine engine{
        app::EngineConfig{.instruments = test::instruments(), .shard_count = shard_count},
        journals.factory(), clock};
    engine.add_subscriber(subscriber);
    engine.start();

    // Several producers race: the interleaving differs from run to run,
    // which is exactly what the journal must capture. Each has its own
    // trader id and its own view of the orders it has seen accepted - kept
    // here, not inside the producer lambda: a completion can still be
    // in flight on the publisher thread after producers.clear() joins the
    // producer threads below (joining only proves they stopped submitting,
    // not that every reply was delivered yet), so anything a completion
    // touches must outlive that join.
    constexpr std::uint64_t producer_count = 3;
    std::vector<test::AcceptedOrders> accepted_per_producer{producer_count};
    std::vector<std::jthread> producers;
    for (std::uint64_t seed = 1; seed <= producer_count; ++seed) {
        producers.emplace_back([&engine, &live_replies, &accepted_per_producer, seed] {
            std::mt19937_64 rng{seed};
            const TraderId trader{seed};
            test::AcceptedOrders& accepted = accepted_per_producer[seed - 1];
            // Copyable, so every (re)submission gets a fresh Completion.
            const auto record = [&live_replies, &accepted](const app::CommandReply& reply) noexcept {
                live_replies.push_back(reply);
                if (reply.result.has_value() && reply.result->order_id.value() != 0) {
                    accepted.record(reply.result->order_id);
                }
            };
            for (int i = 0; i < commands_per_producer; ++i) {
                const Command command =
                    test::maybe_risk_command(rng, trader).value_or(test::random_command(rng, trader, accepted));
                // Retry only on back-pressure; any other refusal is a test bug
                // and must fail loudly rather than spin forever.
                for (;;) {
                    const auto submitted =
                        std::holds_alternative<BlockTrader>(command) ||
                                std::holds_alternative<UnblockTrader>(command) ||
                                std::holds_alternative<KillSwitch>(command)
                            ? engine.broadcast(command)
                            : engine.submit(command, record);
                    if (submitted || submitted.error() != app::SubmitError::Overloaded) {
                        EXPECT_TRUE(submitted.has_value()) << app::to_string(submitted.error());
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    producers.clear();  // join producers, then drain the engine
    ASSERT_TRUE(engine.broadcast(KillSwitch{RiskCommandId{1'000'000}, true}).has_value());
    engine.stop();

    for (std::uint32_t s = 0; s < shard_count; ++s) {
        const ShardId shard{s};
        const auto commands = journals.of(shard).committed();
        ASSERT_FALSE(commands.empty());

        // Replay into a fresh engine configured exactly like the live shard.
        const app::Router router = app::Router::round_robin(test::instruments(), shard_count);
        const auto owned = router.instruments_of(shard);
        ShardEngine fresh{ShardConfig{.shard = shard, .instruments = {owned.begin(), owned.end()}}};
        const app::ReplayOutput replayed = app::replay(fresh, commands);

        const auto live_events =
            subscriber.events() |
            std::views::filter([&](const auto& e) { return e.shard == shard; }) |
            std::ranges::to<std::vector>();
        auto live_shard_replies =
            live_replies | std::views::filter([&](const auto& r) { return r.shard == shard; }) |
            std::ranges::to<std::vector>();

        // Full vector equality, not just counts/digests: the first mismatch
        // is reported at its exact shard sequence number.
        EXPECT_EQ(replayed.events.size(), live_events.size());
        for (const auto& [live, again] : std::views::zip(live_events, replayed.events)) {
            ASSERT_EQ(live, again)
                << "first divergence at shard sequence " << live.sequence.value();
        }
        // Broadcast risk commands have no completion, so replay yields more replies;
        // every live reply must match the replayed reply at the same sequence.
        std::map<std::uint64_t, app::CommandReply> replayed_by_seq;
        for (const auto& reply : replayed.replies) {
            replayed_by_seq.emplace(reply.sequence.value(), reply);
        }
        for (const auto& reply : live_shard_replies) {
            ASSERT_EQ(replayed_by_seq.at(reply.sequence.value()), reply);
        }

        // Final book state must agree too (acceptance criterion 2's "events,
        // replies and book snapshots are identical" applies here as well,
        // since the live engine is still inspectable before Engine's
        // destructor runs).
        for (const InstrumentSpec& spec : owned) {
            const OrderBook* live_book = engine.shard(shard).engine().book(spec.id);
            const OrderBook* replayed_book = fresh.book(spec.id);
            ASSERT_NE(live_book, nullptr);
            ASSERT_NE(replayed_book, nullptr);
            EXPECT_EQ(live_book->snapshot(), replayed_book->snapshot())
                << "instrument " << spec.id.value();
        }
    }
}

}  // namespace
}  // namespace lockstep
