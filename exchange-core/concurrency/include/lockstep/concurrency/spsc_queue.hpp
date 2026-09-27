#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

#include "lockstep/concurrency/cache_aligned.hpp"

namespace lockstep::concurrency {

namespace detail {
template <typename>
inline constexpr bool not_implemented = false;
}  // namespace detail

/// Bounded wait-free single-producer/single-consumer ring buffer (shard ->
/// publisher egress). Design and memory-ordering policy: ADR-0011.
///
/// Layout: the producer-written index and the consumer-written index live on
/// separate cache lines; each side keeps a private cached copy of the other
/// side's index and only re-reads the shared atomic when the cache says the
/// queue is full/empty. Capacity is rounded up to a power of two (index masking).
///
/// SKELETON STATUS: interface and layout only. Instantiating a member function
/// fails at compile time with a pointer to docs/tasks/005-spsc-queue.md.
template <typename T>
class SpscQueue {
public:
    using value_type = T;
    static constexpr bool multi_producer = false;
    static constexpr bool multi_consumer = false;

    explicit SpscQueue(std::size_t capacity) {
        static_assert(detail::not_implemented<T>, "SpscQueue: see docs/tasks/005-spsc-queue.md");
        (void)capacity;
    }

    [[nodiscard]] bool try_push(T&& value) {
        static_assert(detail::not_implemented<T>, "SpscQueue: see docs/tasks/005-spsc-queue.md");
        (void)value;
        return false;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        static_assert(detail::not_implemented<T>, "SpscQueue: see docs/tasks/005-spsc-queue.md");
        return std::nullopt;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
    };

    // Producer line: tail_ is published to the consumer; cached_head_ is producer-private.
    alignas(cache_line_size) std::atomic<std::size_t> tail_{0};
    std::size_t cached_head_{0};
    // Consumer line: head_ is published to the producer; cached_tail_ is consumer-private.
    alignas(cache_line_size) std::atomic<std::size_t> head_{0};
    std::size_t cached_tail_{0};
    // Read-only after construction; shared by both sides without contention.
    alignas(cache_line_size) std::size_t mask_{0};
    std::unique_ptr<Slot[]> slots_;
};

}  // namespace lockstep::concurrency
