#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "lockstep/app/messages.hpp"
#include "lockstep/concurrency/cache_aligned.hpp"
#include "lockstep/concurrency/spsc_queue.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::app {

/// What a Subscription delivers (task 011). Empty `instruments` means every
/// instrument this engine serves. `trades` is Trade, `book_updates` is
/// BookLevelChanged, `private_events` is OrderAccepted, OrderCancelled,
/// OrderModified and RiskCommandApplied (the risk client's view).
/// `InstrumentStatusChanged` is not gated by any of the three booleans: a
/// halt/resume is always delivered, subject only to the instrument filter
/// (docs/tasks/011-publisher-fanout.md's Interfaces section) - matching
/// market_data.proto, which carries no status flag of its own, and the
/// status an order-entry client needs regardless of what else it asked for.
/// RiskCommandApplied carries no instrument (a per-shard broadcast ack), so
/// the instrument filter never excludes it; it is still gated by
/// `private_events`.
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
/// Engine::subscribe/Publisher::subscribe block until registration has
/// actually taken effect before returning this object - never hand back a
/// Subscription whose registration is only queued, not yet applied (see
/// Publisher::subscribe's doc for why and the bounded wait that enforces
/// it). At the moment registration is applied, a per-shard starting point
/// is stamped, and only events with a strictly later per-shard sequence are
/// ever delivered (`wants()`'s doc) - so a subscription never sees any part
/// of a command that was already (even partly) flushed to other
/// subscribers before it registered, and always sees every event of a
/// command that starts afterwards.
///
/// A subscription that overflows or is cancelled is never destroyed from
/// inside Publisher: both the consumer and the publisher hold a shared_ptr
/// (Publisher::subscribe hands out shared ownership), so the object
/// outlives whichever side drops its reference first and is only destroyed
/// once both have. Publisher::prune_subscriptions() can run the last erase
/// on `subscriptions_`, so whatever a consumer captured in on_ready() may be
/// destroyed on the *publisher* thread; its destructor must not block or
/// touch anything only the consumer thread may touch.
class Subscription {
public:
    /// `capacity` is rounded up to a power of two, minimum 2 (SpscQueue's
    /// rule - see its doc).
    Subscription(SubscriptionFilter filter, std::size_t capacity);
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) = delete;
    Subscription& operator=(Subscription&&) = delete;

    /// Pops up to out.size() events; returns how many were written.
    std::size_t poll(std::span<PublishedEvent> out);
    /// True once the buffer overflowed (or the publisher shut down with this
    /// subscription still live - see Publisher::run); no further events are
    /// delivered either way.
    [[nodiscard]] bool overflowed() const noexcept;
    /// Called by the consumer to stop delivery. Idempotent, and blocks (a
    /// short spin, never a park) until any on_ready() invocation already in
    /// progress on the publisher thread has returned, so that once cancel()
    /// returns the hook is guaranteed never to run again - the consumer may
    /// immediately destroy whatever on_ready()'s callback captured (e.g. a
    /// gRPC reactor's OnDone). Must not be called from inside the hook
    /// itself (it would deadlock, spinning on its own completion).
    void cancel() noexcept;
    /// Optional wake-up hook the publisher calls (on its thread) once per
    /// flush that delivers to this subscription or that overflows it; must
    /// not block. Its captured state only has to stay valid until cancel()
    /// returns (cancel()'s doc guarantees the hook never runs again after
    /// that - it does not need to be kept alive indefinitely or shared
    /// beyond that point). May be set only once - set before the first
    /// poll() if the consumer relies on it rather than polling proactively,
    /// since nothing already delivered before the hook is installed re-fires
    /// it. A second call is rejected (returns false) rather than replacing
    /// the hook: the publisher thread may already be executing the first
    /// one, and freeing a function object out from under that call would be
    /// a use-after-free.
    [[nodiscard]] bool on_ready(std::move_only_function<void() noexcept> callback);

    /// True once Publisher::drain_control() has processed this subscription
    /// (or close() has run before that ever happened) - what
    /// Publisher::subscribe blocks on before returning it to its caller, so
    /// that caller never observes a "registered" subscription whose
    /// start_sequence_ could be stale (see set_start_sequence()'s doc).
    [[nodiscard]] bool is_registered() const noexcept;

    // --- Publisher-only below: called only by Publisher, on the publisher
    // thread, and never concurrently with another call to one of these. ---

    /// True once the consumer called cancel(); Publisher drops the
    /// subscription from its list on its next iteration.
    [[nodiscard]] bool cancelled() const noexcept;
    /// Records this subscription's per-shard starting point: `cutoffs[s]` is
    /// the sequence number of the last item Publisher had already popped for
    /// shard `s` at the moment this subscription was drained off the control
    /// queue (Publisher::drain_control()), and marks it registered (see
    /// is_registered()). Called exactly once, before this subscription is
    /// added to Publisher's subscriptions_ list (so before wants() is ever
    /// called on it).
    void set_start_sequence(std::vector<domain::SequenceNumber> cutoffs);
    /// Whether `event` passes this subscription's filter and started after
    /// registration (see set_start_sequence()'s doc and the class comment).
    [[nodiscard]] bool wants(const PublishedEvent& event) const;
    /// Pushes `event` to the ring. Returns false and marks the subscription
    /// overflowed the moment the ring is full; the caller must stop calling
    /// deliver() on this subscription for the rest of the current flush (see
    /// overflowed()'s doc - no further events are delivered after that).
    bool deliver(const PublishedEvent& event);
    /// Invokes the on_ready() hook, if one was set and this subscription is
    /// not cancelled. Call at most once per publisher flush that touched
    /// this subscription: a hook that fired once per event rather than once
    /// per batch could itself become the slow part of a slow consumer
    /// (ADR-0006).
    void notify_ready() noexcept;
    /// Marks this subscription as done (same observable effect as overflow:
    /// no further events, `overflowed()` becomes true) and registered (see
    /// is_registered()) and fires on_ready() once. Called by Publisher::run()
    /// for every subscription still live when the publisher thread is about
    /// to exit, and by Publisher::subscribe for a subscription that is still
    /// waiting to be registered when the publisher has already stopped, so
    /// a consumer blocked in subscribe() or only on this subscription's
    /// events never hangs past shutdown.
    void close() noexcept;

private:
    SubscriptionFilter filter_;
    concurrency::SpscQueue<PublishedEvent> ring_;
    // Per-shard cutoff from set_start_sequence(); empty until that runs
    // (publisher-thread-only: written once there, read only from wants(),
    // both on the publisher thread, so no atomics needed for this field).
    std::vector<domain::SequenceNumber> start_sequence_;
    // Written once by set_start_sequence()/close() (publisher thread),
    // spin-polled by subscribe() (the subscribing thread) - see
    // is_registered()'s doc. release/acquire: publishes start_sequence_
    // (written just before, by the same thread) to whoever observes this
    // flip to true.
    std::atomic<bool> registered_{false};

    // consumer-written: cancel() sets cancelled_ (and spins on notifying_);
    // on_ready() publishes a new hook. Kept off the publisher-written line
    // below (ADR-0011's cache-line rule: these are independently written by
    // different threads under real traffic).
    alignas(concurrency::cache_line_size) std::atomic<bool> cancelled_{false};
    // Heap-allocated so it can be published with one atomic pointer swap;
    // move_only_function itself has no atomic form. Set once (on_ready()
    // rejects a second call - see its doc); freed by whichever of on_ready()
    // (on a lost CAS, never published) or the destructor owns the final
    // value, never while notify_ready() might still be running it.
    std::atomic<std::move_only_function<void() noexcept>*> on_ready_{nullptr};

    // publisher-written: overflowed_ (deliver()/close()) and notifying_ (the
    // cancel()/notify_ready() handshake below).
    alignas(concurrency::cache_line_size) std::atomic<bool> overflowed_{false};
    // Dekker-style handshake so cancel() can guarantee "the hook will never
    // run again" to its caller (see cancel()'s doc): notify_ready() sets
    // notifying_ before it may invoke the callback and clears it only after
    // the call returns; cancel() sets cancelled_ then spins on notifying_.
    // Needs seq_cst on both sides - two independent atomics, and
    // acquire/release alone does not prevent each thread observing only its
    // own write before the other's (the classic Dekker counterexample).
    std::atomic<bool> notifying_{false};
};

}  // namespace lockstep::app
