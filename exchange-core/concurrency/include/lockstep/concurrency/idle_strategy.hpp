#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>

namespace lockstep::concurrency {

/// What a polling thread does between iterations. `idle(work)` is called once
/// per loop with the number of items processed in that iteration; a strategy
/// typically resets when work > 0 and escalates while work == 0.
template <typename S>
concept IdleStrategy = requires(S& strategy, std::size_t work) {
    { strategy.idle(work) } -> std::same_as<void>;
    { strategy.reset() } -> std::same_as<void>;
};

/// Spin -> yield -> short sleep. Good default for a skeleton: low latency when
/// busy, near-zero CPU when idle, no cross-thread wake-up protocol needed.
/// Task 007 adds a parking strategy (std::atomic::wait) with explicit wake-ups.
class BackoffIdle {
public:
    struct Config {
        std::uint32_t spins = 256;
        std::uint32_t yields = 64;
        std::chrono::microseconds sleep{50};
    };

    BackoffIdle() = default;
    explicit BackoffIdle(Config config) noexcept : config_{config} {}

    void idle(std::size_t work) {
        if (work > 0) {
            reset();
            return;
        }
        if (idle_iterations_ < config_.spins) {
            cpu_relax();
        } else if (idle_iterations_ < config_.spins + config_.yields) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(config_.sleep);
        }
        if (idle_iterations_ < config_.spins + config_.yields) {
            ++idle_iterations_;  // saturate: stay in the sleep phase until work arrives
        }
    }

    void reset() noexcept { idle_iterations_ = 0; }

    [[nodiscard]] std::uint32_t idle_iterations() const noexcept { return idle_iterations_; }

private:
    static void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }

    Config config_{};
    std::uint32_t idle_iterations_{0};
};

static_assert(IdleStrategy<BackoffIdle>);

/// A counter consumers park on and producers bump. Producers call ring() after
/// a successful push; consumers call wait(seen, stop) when idle. The contract
/// is that ring() happens-before whatever a waiter observes once wait() (or a
/// value() re-check) reports a change - so ring() must only be called after
/// the state the waiter cares about (e.g. a pushed queue element) is already
/// visible to other threads.
///
/// Built on std::atomic<T>::wait/notify_one rather than a condition
/// variable: the standard specifically designed that pair to close the
/// lost-wakeup window between "check the condition" and "start blocking",
/// which a std::condition_variable can only get by also holding a mutex
/// across both steps - a lock this runtime's shard/publisher threads would
/// otherwise need only for parking (task 007).
class Doorbell {
public:
    [[nodiscard]] std::uint64_t value() const noexcept {
        // acquire: pairs with the release in ring(), so a thread that
        // observes a ring also observes whatever happened-before it.
        return counter_.load(std::memory_order_acquire);
    }

    void ring() noexcept {
        // release: pairs with the acquire in value()/wait(); see the class
        // comment on the happens-before contract this gives callers.
        counter_.fetch_add(1, std::memory_order_release);
        // One waiter per Doorbell in this runtime (one shard thread on its
        // ingress doorbell, one publisher thread on its egress doorbell), so
        // notify_one() is enough; a second ring before the waiter wakes just
        // coalesces into a larger counter delta, which wait()'s "did the
        // value change" check already handles.
        //
        // libstdc++ 14's std::atomic<uint64_t>::wait/notify is not futex-
        // native (that needs a 32-bit word): it goes through a shared pool of
        // 16 proxy waiters keyed by (address >> 2) % 16. Every Doorbell here
        // is cache-line aligned, so all of them land in the same bucket. No
        // wake-up is lost because a notify on a proxy wakes *all* its waiters
        // (bits/atomic_wait.h, __waiter_pool::_M_notify sets __all when the
        // address is the proxy's), and each re-checks its own counter. The
        // cost is a shared seq_cst RMW per ring and spurious wake-ups of other
        // parked threads. Candidate fix, to measure in task 018: keep
        // value()'s uint64_t signature but store a std::atomic<std::uint32_t>
        // counter, which waits on its own address.
        counter_.notify_one();
    }

    /// Blocks until value() != seen, or stop is requested. `seen` must have
    /// been read before the caller's final "is there work" check (e.g. the
    /// try_pop() that found the queue empty) - reading it any later could
    /// race a push that completes (and rings) in between, leaving the caller
    /// to wait on a counter value that already reflects it and will never
    /// change again: a lost wake-up. See MpscQueue's documented stall
    /// property for why a single try_pop() is not enough of a recheck by
    /// itself.
    void wait(std::uint64_t seen, const std::stop_token& stop) const {
        if (stop.stop_requested()) {
            // A stop_callback rings this doorbell on request_stop() (see
            // ShardRuntime::run/Publisher::run), but that ring may already
            // be folded into `seen` if it happened before the caller read
            // it - the same staleness risk as a missed push, so stop needs
            // its own check rather than relying on the counter alone.
            return;
        }
        // acquire: pairs with the release in ring(); see value()'s comment.
        counter_.wait(seen, std::memory_order_acquire);
    }

private:
    std::atomic<std::uint64_t> counter_{0};
};

// Not nested inside ParkingIdle: a default member initializer there cannot
// be used in that class's own constructor's default argument until the
// class is complete (GCC/Clang agree: "default member initializer ...
// required before the end of its enclosing class").
struct ParkingIdleConfig {
    std::uint32_t spins = 256;
    std::uint32_t yields = 64;
};

/// Spin -> yield -> park on a Doorbell. Constructed per run() call with the
/// Doorbell to park on and the stop_token the caller's loop is driven by.
/// `parks_counter`, if given, is bumped (relaxed store) at the moment this
/// strategy enters Doorbell::wait() - not after it returns - so a reader on
/// another thread (e.g. ShardRuntime::stats()) can observe "this thread is
/// now parked" while it is still blocked, rather than only after it wakes.
///
/// Avoiding lost wake-ups: a `seen` value is captured at the end of every
/// idle(0) call, to be used only by the *next* call if that one parks. The
/// run loop always does its "is there work" check (poll_once()) before
/// calling idle(), so a `seen` captured at the end of call N is guaranteed
/// to predate call N+1's check - exactly the ordering Doorbell::wait()
/// requires. Sampling value() fresh inside the park branch itself would
/// instead read it *after* that same call's check, which is too late (see
/// Doorbell::wait()'s comment).
class ParkingIdle {
public:
    using Config = ParkingIdleConfig;

    ParkingIdle(Doorbell& doorbell,
                const std::stop_token& stop,
                std::atomic<std::uint64_t>* parks_counter = nullptr,
                Config config = {}) noexcept
        : doorbell_{&doorbell}, stop_{&stop}, parks_counter_{parks_counter}, config_{config} {}

    void idle(std::size_t work) {
        if (work > 0) {
            reset();
            return;
        }
        if (idle_iterations_ < config_.spins) {
            cpu_relax();
        } else if (idle_iterations_ < config_.spins + config_.yields) {
            std::this_thread::yield();
        } else {
            ++parks_;
            if (parks_counter_ != nullptr) {
                // relaxed: diagnostic only; published before blocking so a
                // reader sees "parked" while this thread is still inside
                // wait(), not only after it returns (see the class comment).
                parks_counter_->store(parks_, std::memory_order_relaxed);
            }
            doorbell_->wait(pending_seen_, *stop_);
        }
        if (idle_iterations_ < config_.spins + config_.yields) {
            ++idle_iterations_;  // saturate: stay in the park phase until work arrives
        }
        // Captured for the *next* call, not this one - see the class comment.
        pending_seen_ = doorbell_->value();
    }

    void reset() noexcept { idle_iterations_ = 0; }

    /// Number of times this strategy has entered Doorbell::wait() during the
    /// park phase. Not necessarily how long (or whether) it actually stayed
    /// blocked - a stop request or a spurious wake can make wait() return
    /// immediately. Single-threaded: only the thread driving idle() may call
    /// this (a caller that needs the count visible to other threads passes
    /// `parks_counter` to the constructor instead, e.g. ShardRuntime).
    [[nodiscard]] std::uint64_t parks() const noexcept { return parks_; }

private:
    static void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }

    Doorbell* doorbell_;
    const std::stop_token* stop_;
    std::atomic<std::uint64_t>* parks_counter_;
    Config config_{};
    std::uint32_t idle_iterations_{0};
    std::uint64_t pending_seen_{0};
    std::uint64_t parks_{0};
};

static_assert(IdleStrategy<ParkingIdle>);

}  // namespace lockstep::concurrency
