// Price-level container choice (ADR-0009): flat_map (sorted contiguous keys)
// versus std::map (node-based red-black tree) for the access pattern of an
// order book - a few dozen live levels, heavy churn near the top of book.
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "lockstep/domain/flat_map.hpp"

#include "percentiles.hpp"

namespace {

using lockstep::domain::flat_map;

struct Level {
    std::uint64_t total{0};
    std::uint32_t orders{0};
};

/// Prices clustered around the touch: most activity is within a few ticks of
/// the best price, as in real books.
std::vector<std::int64_t> clustered_prices(std::size_t count, std::int64_t spread) {
    std::mt19937_64 rng{42};
    std::normal_distribution<double> dist{0.0, static_cast<double>(spread) / 4.0};
    std::vector<std::int64_t> prices(count);
    for (auto& price : prices) {
        price = 10'000 + static_cast<std::int64_t>(dist(rng));
    }
    return prices;
}

template <typename Map>
void BM_LevelUpsertErase(benchmark::State& state) {
    const auto spread = state.range(0);
    const auto prices = clustered_prices(1U << 16U, spread);
    lockstep::bench::LatencySamples samples;
    Map levels;
    std::size_t i = 0;

    for (auto _ : state) {
        const std::int64_t price = prices[i++ & (prices.size() - 1)];
        samples.measure([&] {
            auto [it, inserted] = levels.try_emplace(price);
            Level& level = it->second;
            ++level.orders;
            level.total += 10;
            // Remove every other touched level to keep the book size stable.
            if (level.orders > 1) {
                levels.erase(it);
            }
        });
        benchmark::DoNotOptimize(levels);
    }
    samples.report(state);
    state.counters["levels"] = static_cast<double>(levels.size());
}

template <typename Map>
void BM_BestLevelLookup(benchmark::State& state) {
    Map levels;
    for (std::int64_t p = 0; p < state.range(0); ++p) {
        levels.try_emplace(10'000 - p);
    }
    for (auto _ : state) {
        auto best = levels.begin()->first;
        benchmark::DoNotOptimize(best);
    }
}

using FlatBids = flat_map<std::int64_t, Level, std::greater<>>;
using TreeBids = std::map<std::int64_t, Level, std::greater<>>;

BENCHMARK(BM_LevelUpsertErase<FlatBids>)->Name("upsert_erase/flat_map")->Arg(16)->Arg(64)->Arg(512);
BENCHMARK(BM_LevelUpsertErase<TreeBids>)->Name("upsert_erase/std::map")->Arg(16)->Arg(64)->Arg(512);
BENCHMARK(BM_BestLevelLookup<FlatBids>)->Name("best_level/flat_map")->Arg(64);
BENCHMARK(BM_BestLevelLookup<TreeBids>)->Name("best_level/std::map")->Arg(64);

}  // namespace
