// Deterministic replay (ADR-0004): whatever interleaving the live, multi-threaded
// run happened to take, replaying each shard's journal into a fresh ShardEngine
// must reproduce that shard's events, replies and book state exactly.
//
// SKELETON STATUS: the generator only sends NewOrder/CancelOrder, so the
// event stream now includes fills and rests (task 002 matches) alongside
// rejections, but no modifies or risk commands yet. Task 010 extends the
// generator further and adds file-journal round trips; the structure of the
// property stays the same.
#include <algorithm>
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

namespace lockstep {
namespace {

using namespace domain;

constexpr std::size_t shard_count = 2;
constexpr std::uint32_t instrument_count = 4;

std::vector<InstrumentSpec> instruments() {
    return std::views::iota(1U, instrument_count + 1) | std::views::transform([](std::uint32_t id) {
               return InstrumentSpec{.id = InstrumentId{id}};
           }) |
           std::ranges::to<std::vector>();
}

Command random_command(std::mt19937_64& rng) {
    // Only listed instruments: an unknown one is refused by submit() before it
    // reaches a shard (nothing to replay), which the pipeline tests cover.
    std::uniform_int_distribution<std::uint32_t> instrument{1, instrument_count};
    std::uniform_int_distribution<std::int64_t> price{0, 120};  // 0: domain rejects (journaled)
    std::uniform_int_distribution<std::uint64_t> qty{0, 50};    // 0: domain rejects (journaled)
    std::uniform_int_distribution<int> kind{0, 9};

    const InstrumentId target{instrument(rng)};
    if (kind(rng) == 0) {
        return CancelOrder{TraderId{rng() % 5}, target, OrderId{rng() % 64}};
    }
    return NewOrder{.trader = TraderId{rng() % 5},
                    .client_order_id = ClientOrderId{rng()},
                    .instrument = target,
                    .side = (rng() % 2 == 0) ? Side::Buy : Side::Sell,
                    .type = OrderType::Limit,
                    .time_in_force = TimeInForce::Gtc,
                    .price = Price{price(rng)},
                    .quantity = Quantity{qty(rng)}};
}

// [itest->req~modify.determinism-test-still-passes~1]
TEST(ReplayDeterminism, JournalReplayReproducesLiveOutputPerShard) {
    app::ManualClock clock;
    test::MemoryJournals journals;
    test::RecordingSubscriber subscriber;
    std::vector<app::CommandReply> live_replies;  // written only on the publisher thread

    // The engine owns the journals, so it must outlive the comparison below.
    app::Engine engine{app::EngineConfig{.instruments = instruments(), .shard_count = shard_count},
                       journals.factory(), clock};
    engine.add_subscriber(subscriber);
    engine.start();

    // Several producers race: the interleaving differs from run to run,
    // which is exactly what the journal must capture.
    std::vector<std::jthread> producers;
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        producers.emplace_back([&engine, &live_replies, seed] {
            std::mt19937_64 rng{seed};
            // Copyable, so every (re)submission gets a fresh Completion.
            const auto record = [&live_replies](const app::CommandReply& reply) noexcept {
                live_replies.push_back(reply);
            };
            for (int i = 0; i < 2'000; ++i) {
                const Command command = random_command(rng);
                // Retry only on back-pressure; any other refusal is a test bug
                // and must fail loudly rather than spin forever.
                for (;;) {
                    const auto submitted = engine.submit(command, record);
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
    ASSERT_TRUE(engine.broadcast(KillSwitch{RiskCommandId{1}, true}).has_value());
    engine.stop();

    for (std::uint32_t s = 0; s < shard_count; ++s) {
        const ShardId shard{s};
        const auto commands = journals.of(shard).committed();
        ASSERT_FALSE(commands.empty());

        // Replay into a fresh engine configured exactly like the live shard.
        const app::Router router = app::Router::round_robin(instruments(), shard_count);
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
    }
}

}  // namespace
}  // namespace lockstep
