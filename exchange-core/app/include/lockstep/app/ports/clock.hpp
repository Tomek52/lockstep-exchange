#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "lockstep/domain/types.hpp"

namespace lockstep::app {

/// Outbound port: source of command timestamps. Read ONLY by the shard runtime
/// when it sequences a command; the timestamp is then journaled with the
/// command, so replay never consults a clock (ADR-0004). Called from several
/// shard threads concurrently; implementations must be thread-safe.
class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    [[nodiscard]] virtual domain::Timestamp now() noexcept = 0;
};

class SystemClock final : public Clock {
public:
    [[nodiscard]] domain::Timestamp now() noexcept override {
        const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
        return domain::Timestamp{
            std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count()};
    }
};

/// Deterministic clock for tests: every call returns the previous value + step.
class ManualClock final : public Clock {
public:
    explicit ManualClock(std::int64_t start_ns = 1, std::int64_t step_ns = 1) noexcept
        : next_{start_ns}, step_{step_ns} {}

    [[nodiscard]] domain::Timestamp now() noexcept override {
        // relaxed: callers only need each value to be unique and the counter to
        // be free of torn updates; no other memory is published through it.
        return domain::Timestamp{next_.fetch_add(step_, std::memory_order_relaxed)};
    }

private:
    std::atomic<std::int64_t> next_;
    std::int64_t step_;
};

}  // namespace lockstep::app
