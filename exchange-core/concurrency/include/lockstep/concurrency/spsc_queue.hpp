#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "lockstep/concurrency/cache_aligned.hpp"

namespace lockstep::concurrency {

namespace detail {
template <typename>
inline constexpr bool not_implemented = false;

/// Smallest power of two >= n, and >= 2 (a 1-slot ring cannot distinguish
/// "full" from "empty" under the tail - head == capacity rule below).
[[nodiscard]] constexpr std::size_t round_up_capacity(std::size_t n) noexcept {
    if (n <= 2) {
        return 2;
    }
    return std::bit_ceil(n);
}
}  // namespace detail

/// Bounded wait-free single-producer/single-consumer ring buffer (shard ->
/// publisher egress). Design and memory-ordering policy: ADR-0011.
///
/// Layout: the producer-written index and the consumer-written index live on
/// separate cache lines; each side keeps a private cached copy of the other
/// side's index and only re-reads the shared atomic when the cache says the
/// queue is full/empty. Capacity is rounded up to a power of two (index masking).
///
/// Threading: try_push() only from one producer thread, try_pop() only from
/// one consumer thread. Destruction requires both to have stopped using the
/// queue (e.g. joined). try_push() leaves `value` untouched when it fails.
template <typename T>
class SpscQueue {
public:
    using value_type = T;
    static constexpr bool multi_producer = false;
    static constexpr bool multi_consumer = false;

    explicit SpscQueue(std::size_t capacity)
        : mask_{detail::round_up_capacity(capacity) - 1}, slots_(mask_ + 1) {}

    ~SpscQueue() {
        // Destroy whatever is still queued. Destruction happens after both
        // threads stopped using the queue, and whatever ended that use (e.g.
        // join) already orders their writes before this point, so:
        // relaxed: no concurrent writer of head_ remains
        std::size_t head = head_.load(std::memory_order_relaxed);
        // relaxed: no concurrent writer of tail_ remains
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        while (head != tail) {
            std::destroy_at(
                std::launder(reinterpret_cast<T*>(slots_[head & mask_].storage.data())));
            ++head;
        }
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    [[nodiscard]] bool try_push(T&& value) {
        // relaxed: only this thread writes tail_
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - cached_head_ == capacity()) {
            // Cache says full: refresh from the consumer's published index
            // before giving up, since it may have advanced since we last
            // looked.
            // acquire: pairs with the consumer's release store to head_ in
            // try_pop(), so the slot it just vacated is visible before we
            // reuse it.
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - cached_head_ == capacity()) {
                return false;  // still full: `value` untouched, caller may retry
            }
        }
        std::construct_at(std::launder(reinterpret_cast<T*>(slots_[tail & mask_].storage.data())),
                          std::move(value));
        // release: pairs with the consumer's acquire load of tail_ in
        // try_pop(), publishing the constructed element before the index
        // that makes it visible.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        // relaxed: only this thread writes head_
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            // Cache says empty: refresh from the producer's published index
            // before giving up.
            // acquire: pairs with the producer's release store to tail_ in
            // try_push(), making the pushed element visible before we read it.
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return std::nullopt;
            }
        }
        T* slot = std::launder(reinterpret_cast<T*>(slots_[head & mask_].storage.data()));
        std::optional<T> out{std::move(*slot)};
        std::destroy_at(slot);
        // release: pairs with the producer's acquire load of head_ in
        // try_push(), so the vacated slot is visible before the producer
        // reuses it.
        head_.store(head + 1, std::memory_order_release);
        return out;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Slot {
        alignas(T) std::array<std::byte, sizeof(T)> storage;
    };

    // Producer line: tail_ is published to the consumer; cached_head_ is producer-private.
    alignas(cache_line_size) std::atomic<std::size_t> tail_{0};
    std::size_t cached_head_{0};
    // Consumer line: head_ is published to the producer; cached_tail_ is consumer-private.
    alignas(cache_line_size) std::atomic<std::size_t> head_{0};
    std::size_t cached_tail_{0};
    // Read-only after construction; shared by both sides without contention.
    alignas(cache_line_size) std::size_t mask_{0};
    std::vector<Slot> slots_;
};

}  // namespace lockstep::concurrency
