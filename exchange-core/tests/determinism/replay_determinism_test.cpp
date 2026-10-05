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
#include <ranges>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/app/replay.hpp"

#include "app/test_support.hpp"
#include "generator.hpp"

namespace lockstep {
namespace {

using namespace domain;

constexpr std::size_t shard_count = 2;

TEST(ReplayDeterminism, JournalReplayReproducesLiveOutputPerShard) {
    app::ManualClock clock;
    test::MemoryJournals journals;
    test::RecordingSubscriber subscriber;
    std::vector<app::CommandReply> live_replies;  // written only on the publisher thread

    // The engine owns the journals, so it must outlive the comparison below.
    app::Engine engine{
        app::EngineConfig{.instruments = test::instruments(), .shard_count = shard_count},
        journals.factory(), clock};
    test::run_workload(engine, subscriber, live_replies);

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

        // Final book state must agree too: the live engine is still
        // inspectable before Engine's destructor runs.
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
