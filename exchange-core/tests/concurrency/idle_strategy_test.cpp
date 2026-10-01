#include "lockstep/concurrency/idle_strategy.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lockstep/concurrency/mpsc_queue.hpp"

namespace lockstep::concurrency {
namespace {

TEST(BackoffIdle, EscalatesWhileIdleAndResetsOnWork) {
    BackoffIdle idle{{.spins = 2, .yields = 2, .sleep = std::chrono::microseconds{1}}};
    for (int i = 0; i < 3; ++i) {
        idle.idle(0);
    }
    EXPECT_EQ(idle.idle_iterations(), 3U);

    idle.idle(1);
    EXPECT_EQ(idle.idle_iterations(), 0U);
}

TEST(BackoffIdle, SaturatesInSleepPhase) {
    BackoffIdle idle{{.spins = 1, .yields = 1, .sleep = std::chrono::microseconds{1}}};
    for (int i = 0; i < 10; ++i) {
        idle.idle(0);
    }
    EXPECT_EQ(idle.idle_iterations(), 2U);
}

// ---- Doorbell (task 007) ---------------------------------------------------

TEST(Doorbell, WaitReturnsPromptlyAfterRingFromAnotherThread) {
    Doorbell doorbell;
    std::stop_source never_stops;
    const auto seen = doorbell.value();

    const auto start = std::chrono::steady_clock::now();
    std::jthread ringer([&doorbell] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        doorbell.ring();
    });
    doorbell.wait(seen, never_stops.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Generous bound: a lost wake-up (or a fixed-sleep fallback) would make
    // this take far longer, or hang outright.
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
}

TEST(Doorbell, WaitReturnsWhenStopRequested) {
    Doorbell doorbell;
    std::stop_source source;
    const std::stop_token stop = source.get_token();
    // The protocol this doorbell relies on: a stop_callback rings it so a
    // parked waiter re-checks promptly (ShardRuntime::run/Publisher::run do
    // the same thing against the real run loop).
    const std::stop_callback wake_on_stop{stop, [&doorbell] { doorbell.ring(); }};

    std::atomic<bool> returned{false};
    std::jthread waiter([&] {
        const auto seen = doorbell.value();
        doorbell.wait(seen, stop);
        returned.store(true, std::memory_order_release);
        returned.notify_all();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{20});  // let it start parking
    const auto start = std::chrono::steady_clock::now();
    source.request_stop();
    returned.wait(false, std::memory_order_acquire);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
}

TEST(Doorbell, NoLostWakeupInPingPongRounds) {
    constexpr int rounds = 100'000;
    Doorbell ping;
    Doorbell pong;
    std::stop_source never_stops;
    const std::stop_token stop = never_stops.get_token();

    // Both doorbells start at 0 and nothing rings them before the loops
    // below, so 0 is a valid `seen` baseline for both threads - sampling it
    // via value() instead, after the responder thread actually starts
    // running, would race main's first ring() (thread creation does not
    // order the new thread's first statement against the creator's
    // subsequent ones) and could silently fold that first ring into the
    // "already seen" baseline.
    std::jthread responder([&] {
        std::uint64_t seen_ping = 0;
        for (int i = 0; i < rounds; ++i) {
            ping.wait(seen_ping, stop);
            seen_ping = ping.value();
            pong.ring();
        }
    });

    std::uint64_t seen_pong = 0;
    for (int i = 0; i < rounds; ++i) {
        ping.ring();
        pong.wait(seen_pong, stop);
        seen_pong = pong.value();
    }
    // responder's jthread destructor joins; a lost wake-up on either side
    // hangs the test (CTest's timeout is the backstop).
}

TEST(Doorbell, RingMakesPriorWritesVisibleToWaiter) {
    // Exercises ring()/value()/wait()'s own release-acquire pairing in
    // isolation, with no queue alongside it to (redundantly) supply the
    // synchronisation instead: `payload` is ordinary memory, so TSan flags a
    // race here if that pairing is ever weakened to relaxed.
    Doorbell doorbell;
    std::stop_source never_stops;
    const std::stop_token stop = never_stops.get_token();
    int payload = 0;
    const auto seen = doorbell.value();

    std::jthread producer([&] {
        payload = 42;
        doorbell.ring();
    });

    while (doorbell.value() == seen) {
        doorbell.wait(seen, stop);
    }
    EXPECT_EQ(payload, 42);
}

TEST(ParkingIdle, WakesEvenWhenThePushLandsRightBeforeTheParkingCall) {
    // Deterministic regression test for ParkingIdle's class-comment rule:
    // pending_seen_ must be captured at the end of the *previous* idle(0)
    // call, not sampled fresh inside the call that parks. Models a push
    // (and its ring()) landing in the gap between a run loop's "is there
    // work" check and the idle() call that follows it - exactly the window
    // Doorbell::wait()'s comment warns is too late to sample `seen` in.
    Doorbell doorbell;
    std::stop_source never_stops;
    const std::stop_token stop = never_stops.get_token();
    ParkingIdle idle{doorbell, stop, /*parks_counter=*/nullptr,
                     ParkingIdle::Config{.spins = 0, .yields = 0}};

    // Establishes pending_seen_ at the current (pre-ring) counter value;
    // nothing to wait for yet since nothing has rung.
    doorbell.ring();
    idle.idle(0);

    // Lands strictly before the next idle() call, in the window a run
    // loop's own poll (not modelled here) would have already missed.
    doorbell.ring();

    std::atomic<bool> returned{false};
    std::jthread consumer([&] {
        idle.idle(0);  // must not block: pending_seen_ predates this ring
        returned.store(true, std::memory_order_release);
        returned.notify_all();
    });

    const auto start = std::chrono::steady_clock::now();
    returned.wait(false, std::memory_order_acquire);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
}

// ---- ParkingIdle / Doorbell stress (task 007) ------------------------------

// Replicates the shard-runtime shape (MpscQueue ingress + Doorbell + a
// ParkingIdle-driven consumer loop) under concurrent producers, including
// MpscQueue's documented stall property: a producer preempted between
// claiming its slot and publishing it makes try_pop() report "empty" while
// later items already queued behind it. If ParkingIdle ever parked using a
// `seen` sampled too late (after that same iteration's try_pop()), a push
// landing in the gap would be lost and this test would hang.
//
// Producers pause briefly between bursts (instead of pushing flat-out) so
// the consumer actually drains to empty and reaches the park phase between
// bursts - spins=yields=0 forces every empty check straight to
// Doorbell::wait() too, so this exercises real parking, not just spin/yield
// escalation that never blocks. The name doesn't mention "stop": shutdown
// while parked is covered separately by ShutsDownPromptlyWhileParked below.
TEST(ParkingIdle, NoLostWakeupUnderBurstyConcurrentPush) {
    constexpr int producers = 4;
    constexpr int bursts = 50;
    constexpr int per_burst = 100;
    constexpr int total = producers * bursts * per_burst;

    MpscQueue<int> queue{64};  // small on purpose: forces producers to retry/interleave
    Doorbell doorbell;
    std::stop_source source;
    const std::stop_token stop = source.get_token();

    std::vector<std::jthread> producer_threads;
    producer_threads.reserve(producers);
    for (int p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&queue, &doorbell] {
            for (int b = 0; b < bursts; ++b) {
                for (int i = 0; i < per_burst; ++i) {
                    while (!queue.try_push(1)) {
                        std::this_thread::yield();
                    }
                    doorbell.ring();  // after the push, per Doorbell's contract
                }
                std::this_thread::sleep_for(std::chrono::microseconds{200});
            }
        });
    }

    int received = 0;
    ParkingIdle idle{doorbell, stop, /*parks_counter=*/nullptr,
                     ParkingIdle::Config{.spins = 0, .yields = 0}};
    while (received < total) {
        std::optional<int> item = queue.try_pop();
        if (item) {
            idle.idle(1);
            ++received;
        } else {
            idle.idle(0);
        }
    }
    EXPECT_EQ(received, total);
    for (std::jthread& producer : producer_threads) {
        producer.join();
    }
}

TEST(ParkingIdle, ShutsDownPromptlyWhileParked) {
    MpscQueue<int> queue{64};
    Doorbell doorbell;
    std::stop_source source;
    const std::stop_token stop = source.get_token();
    const std::stop_callback wake_on_stop{stop, [&doorbell] { doorbell.ring(); }};

    std::atomic<bool> stopped{false};
    std::jthread consumer([&] {
        ParkingIdle idle{doorbell, stop};
        while (!stop.stop_requested()) {
            idle.idle(queue.try_pop().has_value() ? 1 : 0);
        }
        stopped.store(true, std::memory_order_release);
        stopped.notify_all();
    });

    // No producer ever pushes: the consumer must park, then wake on stop
    // alone (exercising request_stop()'s stop_callback, not a work ring).
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto start = std::chrono::steady_clock::now();
    source.request_stop();
    stopped.wait(false, std::memory_order_acquire);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds{200});
}

}  // namespace
}  // namespace lockstep::concurrency
