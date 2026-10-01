#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "lockstep/concurrency/cache_aligned.hpp"
#include "lockstep/concurrency/spsc_queue.hpp"  // detail::round_up_capacity

namespace lockstep::concurrency {

/// Bounded lock-free multi-producer/single-consumer queue (gRPC threads and the
/// risk client -> shard ingress). Dmitry Vyukov's bounded queue: each slot
/// carries a sequence number that tells producers and the consumer whether the
/// slot is free or full for the current lap, so producers contend only on one
/// CAS of the enqueue cursor. Design and memory orderings: ADR-0011.
template <typename T>
class MpscQueue {
    // try_pop() publishes the slot's sequence before returning its local; a
    // throwing move there (no NRVO) would lose an element already removed
    // from the ring.
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "MpscQueue requires a nothrow-move-constructible T");

public:
    using value_type = T;
    static constexpr bool multi_producer = true;
    static constexpr bool multi_consumer = false;

    explicit MpscQueue(std::size_t capacity)
        : mask_{detail::round_up_capacity(capacity) - 1}, slots_(mask_ + 1) {
        // Lap 0: slot i is free for the first push once its sequence equals
        // its own index (see push()'s "seq == pos" claim condition).
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            // relaxed: no other thread can see slots_ before construction completes
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MpscQueue() {
        // Destroy whatever is still queued. Destruction happens after every
        // producer and the consumer stopped using the queue, and whatever
        // ended that use (e.g. join) already orders their writes before this
        // point, so:
        // relaxed: no concurrent writer of enqueue_pos_/dequeue_pos_ remains
        std::size_t pos = dequeue_pos_;
        const std::size_t end = enqueue_pos_.load(std::memory_order_relaxed);
        while (pos != end) {
            T* element = std::launder(reinterpret_cast<T*>(slots_[pos & mask_].storage.data()));
            std::destroy_at(element);
            ++pos;
        }
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    [[nodiscard]] bool try_push(T&& value) {
        // relaxed: just a starting guess for pos; the CAS below is the
        // actual synchronisation point for claiming a slot.
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & mask_];
            // acquire: pairs with the consumer's release store to
            // sequence in try_pop(), so a slot the consumer just freed
            // (and the destructor that ran on it) is visible before we
            // reuse it.
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                // Slot looks free for this lap: try to claim it. Losing the
                // race just means another producer got there first; reload
                // pos (compare_exchange_weak already did) and retry.
                // relaxed: the claim itself carries no data; the handoff to
                // the consumer is the release store to sequence below.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    std::construct_at(reinterpret_cast<T*>(slot.storage.data()), std::move(value));
                    // release: pairs with the consumer's acquire load of
                    // sequence in try_pop(), publishing the constructed
                    // element before the index that makes it visible.
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed: pos was refreshed to the current enqueue_pos_
                // by compare_exchange_weak; retry with it.
            } else if (diff < 0) {
                return false;  // queue full: `value` untouched, caller may retry
            } else {
                // Another producer has already moved enqueue_pos_ further
                // than our stale pos; reload and retry.
                // relaxed: see the initial load above
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] std::optional<T> try_pop() {
        Slot& slot = slots_[dequeue_pos_ & mask_];
        // acquire: pairs with the producer's release store to sequence in
        // try_push(), making the pushed element visible before we read it.
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        const auto diff =
            static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(dequeue_pos_ + 1);
        if (diff != 0) {
            return std::nullopt;  // empty: no producer has published this slot yet
        }
        T* element = std::launder(reinterpret_cast<T*>(slot.storage.data()));
        std::optional<T> out{std::move(*element)};
        std::destroy_at(element);
        // release: pairs with a producer's acquire load of sequence in
        // try_push(), so the vacated slot (and the destructor that just ran
        // on it) is visible before a producer reuses it next lap.
        slot.sequence.store(dequeue_pos_ + capacity(), std::memory_order_release);
        ++dequeue_pos_;  // consumer-private: only this thread writes it
        return out;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Slot {
        std::atomic<std::size_t> sequence;
        alignas(T) std::array<std::byte, sizeof(T)> storage{};
    };

    // Contended line: enqueue_pos_ is written by every producer via CAS.
    alignas(cache_line_size) std::atomic<std::size_t> enqueue_pos_{0};
    // Consumer-private line: only the single consumer thread touches this.
    alignas(cache_line_size) std::size_t dequeue_pos_{0};
    // Read-only after construction; shared by producers and the consumer
    // without contention.
    alignas(cache_line_size) std::size_t mask_{0};
    std::vector<Slot> slots_;
};

}  // namespace lockstep::concurrency
