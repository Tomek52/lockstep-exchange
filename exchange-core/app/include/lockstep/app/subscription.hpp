#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "lockstep/app/messages.hpp"
#include "lockstep/concurrency/spsc_queue.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::app {

/// What a Subscription delivers (task 011). Empty `instruments` means every
/// instrument this engine serves. The three booleans gate PublishedEvent's
/// domain event kinds: `trades` is Trade; `book_updates` is BookLevelChanged
/// and InstrumentStatusChanged (both public market-structure data, not
/// scoped to one trader); `private_events` is OrderAccepted, OrderCancelled,
/// OrderModified and RiskCommandApplied (the risk client's view).
struct SubscriptionFilter {
    std::vector<domain::InstrumentId> instruments;  // empty = all
    bool trades{true};
    bool book_updates{true};
    bool private_events{
        false};  // OrderAccepted/Cancelled/Modified, RiskCommandApplied (risk client)
};

/// Consumer end of a subscription registered through Engine::subscribe
/// (task 011, ADR-0006's slow-consumer policy). The publisher thread is the
/// single producer into `ring_` and the only caller of the publisher-only
/// members below; exactly one consumer thread (e.g. a gRPC reactor) may call
/// the public members concurrently with that, which is the same threading
/// shape SpscQueue itself requires.
///
/// A subscription that overflows or is cancelled is never destroyed from
/// inside Publisher: both the consumer and the publisher hold a shared_ptr
/// (Engine::subscribe hands out shared ownership), so the object outlives
/// whichever side drops its reference first and is only destroyed once both
/// have.
class Subscription {
public:
    Subscription(SubscriptionFilter filter, std::size_t capacity);
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) = delete;
    Subscription& operator=(Subscription&&) = delete;

    /// Pops up to out.size() events; returns how many were written.
    std::size_t poll(std::span<PublishedEvent> out);
    /// True once the buffer overflowed; no further events are delivered.
    [[nodiscard]] bool overflowed() const noexcept;
    /// Called by the consumer to stop delivery (idempotent).
    void cancel() noexcept;
    /// Optional wake-up hook the publisher calls (on its thread) after
    /// delivering; must not block. Set before first poll.
    void on_ready(std::move_only_function<void() noexcept> callback);

    // --- Publisher-only below: called only by Publisher, on the publisher
    // thread, and never concurrently with another call to one of these. ---

    /// True once the consumer called cancel(); Publisher drops the
    /// subscription from its list on its next iteration.
    [[nodiscard]] bool cancelled() const noexcept;
    /// Whether `event` passes this subscription's filter.
    [[nodiscard]] bool wants(const PublishedEvent& event) const;
    /// Pushes `event` to the ring. Returns false and marks the subscription
    /// overflowed the moment the ring is full; the caller must stop calling
    /// deliver() on this subscription for the rest of the current flush (see
    /// overflowed()'s doc - no further events are delivered after that).
    bool deliver(const PublishedEvent& event);
    /// Invokes the on_ready() hook, if one was set. Call at most once per
    /// publisher flush that touched this subscription: a hook that fired
    /// once per event rather than once per batch could itself become the
    /// slow part of a slow consumer (ADR-0006).
    void notify_ready() noexcept;

private:
    SubscriptionFilter filter_;
    concurrency::SpscQueue<PublishedEvent> ring_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> overflowed_{false};
    // Heap-allocated so it can be published with one atomic pointer swap;
    // move_only_function itself has no atomic form. Owned by this object:
    // on_ready() frees a replaced callback, the destructor frees the last one.
    std::atomic<std::move_only_function<void() noexcept>*> on_ready_{nullptr};
};

}  // namespace lockstep::app
