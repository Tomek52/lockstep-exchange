#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

#include "lockstep/concurrency/cache_aligned.hpp"
#include "lockstep/concurrency/spsc_queue.hpp"  // detail::not_implemented

namespace lockstep::concurrency {

/// Bounded lock-free multi-producer/single-consumer queue (gRPC threads and the
/// risk client -> shard ingress). Dmitry Vyukov's bounded queue: each slot
/// carries a sequence number that tells producers and the consumer whether the
/// slot is free or full for the current lap, so producers contend only on one
/// CAS of the enqueue cursor. Design and memory orderings: ADR-0011.
///
/// SKELETON STATUS: interface and layout only. Instantiating a member function
/// fails at compile time with a pointer to docs/tasks/006-mpsc-queue.md.
template <typename T>
class MpscQueue {
public:
    using value_type = T;
    static constexpr bool multi_producer = true;
    static constexpr bool multi_consumer = false;

    explicit MpscQueue(std::size_t capacity) {
        static_assert(detail::not_implemented<T>, "MpscQueue: see docs/tasks/006-mpsc-queue.md");
        (void)capacity;
    }

    [[nodiscard]] bool try_push(T&& value) {
        static_assert(detail::not_implemented<T>, "MpscQueue: see docs/tasks/006-mpsc-queue.md");
        (void)value;
        return false;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        static_assert(detail::not_implemented<T>, "MpscQueue: see docs/tasks/006-mpsc-queue.md");
        return std::nullopt;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Slot {
        std::atomic<std::size_t> sequence;
        alignas(T) std::byte storage[sizeof(T)];
    };

    alignas(cache_line_size) std::atomic<std::size_t> enqueue_pos_{0};  // contended by producers
    alignas(cache_line_size) std::size_t dequeue_pos_{0};               // consumer-private
    alignas(cache_line_size) std::size_t mask_{0};
    std::unique_ptr<Slot[]> slots_;
};

}  // namespace lockstep::concurrency
