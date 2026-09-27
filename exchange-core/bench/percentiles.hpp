#pragma once

// Per-operation latency sampling for Google Benchmark. Google Benchmark reports
// mean time per iteration; for an exchange the tail matters, so we time each
// operation individually and export p50/p99/p99.9 as counters.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include <benchmark/benchmark.h>

namespace lockstep::bench {

class LatencySamples {
public:
    explicit LatencySamples(std::size_t reserve = 1U << 20U) { samples_.reserve(reserve); }

    template <typename Fn>
    void measure(Fn&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto stop = std::chrono::steady_clock::now();
        samples_.push_back(std::chrono::duration<double, std::nano>(stop - start).count());
    }

    /// Adds p50/p99/p99.9 (nanoseconds) to the benchmark's counters.
    void report(benchmark::State& state) {
        if (samples_.empty()) {
            return;
        }
        std::ranges::sort(samples_);
        const auto at = [this](double q) {
            const auto index =
                static_cast<std::size_t>(q * static_cast<double>(samples_.size() - 1));
            return samples_[index];
        };
        state.counters["p50_ns"] = at(0.50);
        state.counters["p99_ns"] = at(0.99);
        state.counters["p99.9_ns"] = at(0.999);
    }

private:
    std::vector<double> samples_;
};

}  // namespace lockstep::bench
