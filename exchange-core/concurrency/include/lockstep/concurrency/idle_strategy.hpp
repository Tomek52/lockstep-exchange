#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
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

}  // namespace lockstep::concurrency
