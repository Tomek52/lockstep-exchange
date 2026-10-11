// Queue benchmarks (ADR-0011): ping-pong round trip between two threads and
// multi-producer contention. Set LOCKSTEP_BENCH_PIN=1 to pin the threads to
// distinct CPUs (see pinning.hpp); without it the OS places them freely and
// tail latencies are noisier, especially under WSL2.
//
// Every queue is wait-free to poll, so the waiting side spins. These
// benchmarks need at least two (ping-pong) or producers + 1 (contention)
// hardware threads, otherwise they measure the scheduler, not the queue.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "lockstep/concurrency/mpsc_queue.hpp"
#include "lockstep/concurrency/mutex_queue.hpp"
#include "lockstep/concurrency/spsc_queue.hpp"

#include "percentiles.hpp"
#include "pinning.hpp"

namespace {

using lockstep::bench::LatencySamples;
using lockstep::concurrency::MpscQueue;
using lockstep::concurrency::MutexQueue;
using lockstep::concurrency::SpscQueue;

constexpr std::size_t queue_capacity = 4096;

/// One thread sends a value and waits for the echo; the other echoes. The
/// sample is the full round trip (two queue hops), so one hop is about half.
template <typename Queue>
void bm_ping_pong(benchmark::State& state) {
    Queue ping{queue_capacity};
    Queue pong{queue_capacity};
    std::jthread echo{[&ping, &pong](const std::stop_token& stop) {
        const lockstep::bench::ScopedPin pin{1};
        while (!stop.stop_requested()) {
            if (auto value = ping.try_pop()) {
                while (!pong.try_push(std::move(*value))) {
                }
            }
        }
    }};
    const lockstep::bench::ScopedPin pin{0};

    LatencySamples samples;
    std::uint64_t next = 1;
    for (auto _ : state) {
        samples.measure([&] {
            const std::uint64_t value = next++;
            while (!ping.try_push(std::uint64_t{value})) {
            }
            for (;;) {
                if (auto echoed = pong.try_pop()) {
                    benchmark::DoNotOptimize(*echoed);
                    break;
                }
            }
        });
    }
    samples.report(state);
    state.counters["pinned"] = lockstep::bench::pinning_enabled() ? 1.0 : 0.0;
}

/// `producers` threads each push `per_producer` values into one queue that the
/// benchmark thread drains. Reports throughput (items/s over the wall time
/// from the start signal to the last item popped) and the latency of one
/// enqueue, retries included when the queue is momentarily full.
template <typename Queue>
void bm_contended(benchmark::State& state) {
    const auto producers = static_cast<std::size_t>(state.range(0));
    constexpr std::size_t per_producer = 200'000;
    const lockstep::bench::ScopedPin pin{0};

    LatencySamples all{producers * per_producer};
    for (auto _ : state) {
        Queue queue{queue_capacity};
        std::latch ready{static_cast<std::ptrdiff_t>(producers)};
        std::latch start{1};
        std::vector<LatencySamples> per_thread;
        per_thread.reserve(producers);
        for (std::size_t i = 0; i < producers; ++i) {
            per_thread.emplace_back(per_producer);
        }
        std::vector<std::jthread> threads;
        threads.reserve(producers);
        for (std::size_t i = 0; i < producers; ++i) {
            threads.emplace_back([&queue, &ready, &start, &samples = per_thread[i], i] {
                const lockstep::bench::ScopedPin producer_pin{i + 1};
                ready.count_down();
                start.wait();
                for (std::uint64_t n = 0; n < per_producer; ++n) {
                    samples.measure([&] {
                        // A fresh temporary per attempt: try_push leaves its
                        // argument untouched on failure, but this keeps the
                        // retry loop free of a moved-from variable.
                        const std::uint64_t item = n;
                        while (!queue.try_push(std::uint64_t{item})) {
                        }
                    });
                }
            });
        }
        ready.wait();
        const auto begin = std::chrono::steady_clock::now();
        start.count_down();
        std::size_t popped = 0;
        const std::size_t total = producers * per_producer;
        while (popped < total) {
            if (auto value = queue.try_pop()) {
                benchmark::DoNotOptimize(*value);
                ++popped;
            }
        }
        const auto end = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - begin).count());
        threads.clear();  // join before the queue goes out of scope
        for (const LatencySamples& samples : per_thread) {
            all.append(samples);
        }
    }
    all.report(state);
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(producers * per_producer));
    state.counters["producers"] = static_cast<double>(producers);
    state.counters["pinned"] = lockstep::bench::pinning_enabled() ? 1.0 : 0.0;
}

BENCHMARK(bm_ping_pong<MutexQueue<std::uint64_t>>)
    ->Name("queue/ping_pong/mutex")
    ->Iterations(200'000);
BENCHMARK(bm_ping_pong<SpscQueue<std::uint64_t>>)
    ->Name("queue/ping_pong/spsc")
    ->Iterations(200'000);
BENCHMARK(bm_ping_pong<MpscQueue<std::uint64_t>>)
    ->Name("queue/ping_pong/mpsc")
    ->Iterations(200'000);

BENCHMARK(bm_contended<MpscQueue<std::uint64_t>>)
    ->Name("queue/contended/mpsc")
    ->ArgName("producers")
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->UseManualTime()
    ->Iterations(5);
BENCHMARK(bm_contended<MutexQueue<std::uint64_t>>)
    ->Name("queue/contended/mutex")
    ->ArgName("producers")
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->UseManualTime()
    ->Iterations(5);

}  // namespace
