// In-process Engine::submit -> completion round trip (no gRPC).
//
// Closed loop: one command in flight at a time, so the figure is latency, not
// throughput. A round trip crosses the real threads: caller -> MPSC ingress ->
// shard thread (journal append + commit, match) -> SPSC egress -> publisher
// thread -> completion. The waiting caller spins. The engine's own threads use
// their normal ParkingIdle, which spins, then yields, then parks; because the
// next command arrives about a microsecond after the previous completion, the
// threads are still in their spin phase, so this does NOT measure the cost of
// waking a parked thread (a command that arrives after an idle gap pays that
// on top).
//
// With two shards the instruments are dealt round-robin (Router), and the
// benchmark alternates between them. Because only one command is in flight,
// this shows the cost of using a second shard thread, not parallel speed-up.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <benchmark/benchmark.h>
#include <unistd.h>

#include "lockstep/app/engine.hpp"
#include "lockstep/app/ports/clock.hpp"
#include "lockstep/app/ports/journal.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/types.hpp"
#include "lockstep/journal/file_journal_writer.hpp"
#include "lockstep/journal/memory_journal.hpp"

#include "percentiles.hpp"
#include "pinning.hpp"

namespace {

using namespace lockstep;

enum class JournalKind : std::uint8_t { Null, FileNoSync, FileSyncEveryCommit };

/// A scratch directory removed on destruction; file journals live here.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("lockstep-bench-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(path_);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;
    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

app::JournalFactory journal_factory(JournalKind kind,
                                    const ScratchDir& dir,
                                    std::uint32_t shard_count) {
    return [kind, &dir, shard_count](domain::ShardId shard) -> std::unique_ptr<app::Journal> {
        if (kind == JournalKind::Null) {
            return std::make_unique<journal::NullJournal>();
        }
        const auto policy = kind == JournalKind::FileSyncEveryCommit
                                ? journal::SyncPolicy::EveryCommit
                                : journal::SyncPolicy::None;
        // config_hash 0: nothing reads this journal back in a benchmark.
        return journal::FileJournalWriter::create(
            dir.path(), journal::FileHeader{.shard = shard, .shard_count = shard_count}, policy);
    };
}

domain::NewOrder limit(domain::InstrumentId instrument,
                       domain::Side side,
                       std::uint64_t client_id) {
    return domain::NewOrder{.trader = domain::TraderId{1},
                            .client_order_id = domain::ClientOrderId{client_id},
                            .instrument = instrument,
                            .side = side,
                            .type = domain::OrderType::Limit,
                            .time_in_force = domain::TimeInForce::Gtc,
                            .price = domain::Price{100},
                            .quantity = domain::Quantity{1}};
}

/// Arg: shard count. Alternates a resting sell and a crossing
/// buy per instrument, so each pair produces one trade and the book is empty
/// again; every submit is one sample.
template <JournalKind Kind>
void bm_submit_round_trip(benchmark::State& state) {
    const auto shards = static_cast<std::size_t>(state.range(0));

    app::EngineConfig config;
    config.shard_count = shards;
    for (std::uint32_t id = 1; id <= shards; ++id) {
        config.instruments.push_back(domain::InstrumentSpec{.id = domain::InstrumentId{id}});
    }
    const ScratchDir dir;
    app::SystemClock clock;
    app::Engine engine{std::move(config), journal_factory(Kind, dir, static_cast<std::uint32_t>(shards)),
                       clock};
    engine.start();
    const lockstep::bench::ScopedPin pin{0};

    std::atomic<bool> done{false};
    bool ok = true;
    bench::LatencySamples samples;
    std::uint64_t client_id = 0;
    std::size_t step = 0;

    for (auto _ : state) {
        const domain::InstrumentId instrument{static_cast<std::uint32_t>(1 + ((step / 2) % shards))};
        const domain::Side side = (step % 2 == 0) ? domain::Side::Sell : domain::Side::Buy;
        ++step;
        const domain::Command command = limit(instrument, side, ++client_id);
        samples.measure([&] {
            done.store(false);
            const auto submitted = engine.submit(command, [&done, &ok](const app::CommandReply& reply) noexcept {
                if (!reply.result) {
                    ok = false;  // read only after the loop; published by done.store below
                }
                done.store(true);
            });
            if (!submitted) {
                ok = false;
                return;
            }
            while (!done.load()) {
            }
        });
    }
    engine.stop();
    if (!ok) {
        state.SkipWithError("a command was rejected or refused");
        return;
    }
    samples.report(state);
    state.counters["shards"] = static_cast<double>(shards);
    state.counters["pinned"] = lockstep::bench::pinning_enabled() ? 1.0 : 0.0;
}

BENCHMARK(bm_submit_round_trip<JournalKind::Null>)
    ->Name("engine/submit_round_trip/null_journal")
    ->ArgName("shards")
    ->Arg(1)
    ->Arg(2)
    ->Iterations(50'000);
BENCHMARK(bm_submit_round_trip<JournalKind::FileNoSync>)
    ->Name("engine/submit_round_trip/file_journal")
    ->ArgName("shards")
    ->Arg(1)
    ->Arg(2)
    ->Iterations(50'000);
BENCHMARK(bm_submit_round_trip<JournalKind::FileSyncEveryCommit>)
    ->Name("engine/submit_round_trip/file_journal_fsync")
    ->ArgName("shards")
    ->Arg(1)
    ->Iterations(2'000);

}  // namespace
