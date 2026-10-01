#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <vector>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/ports/event_subscriber.hpp"
#include "lockstep/app/queues.hpp"
#include "lockstep/app/subscription.hpp"
#include "lockstep/concurrency/idle_strategy.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::app {

/// The single consumer of every shard's egress queue. Fans published events out
/// to subscribers and runs completions (command replies) - so neither the shard
/// threads nor the domain ever call into gRPC (ADR-0003).
///
/// Per source, a command's events are delivered to subscribers before its
/// reply's completion runs, so a client that sees an ack can rely on the
/// corresponding market data having been handed to the transport.
///
/// Two subscriber shapes: EventSubscriber (add_subscriber(), registered
/// before start, called synchronously - the risk client's path today) and
/// Subscription (subscribe(), registered at runtime from any thread, each
/// with its own bounded buffer and the slow-consumer policy - task 011).
///
/// Registering a Subscription without splitting a command (task 011 code
/// review): `release_staged()` (ShardRuntime) pushes one command's events to
/// egress one try_push() at a time, not as a single atomic unit, so this
/// thread's try_pop() can observe "nothing more right now" partway through a
/// command, and later see the rest once the shard catches up. A Subscription
/// registered in that gap must see either *all* of that command's events or
/// *none* of them, never just the tail. Every PublishedEvent already carries
/// its command's per-shard `sequence` (events.hpp/messages.hpp), so this is
/// solved without buffering further than `pending_` already does: `drain_control()`
/// runs immediately before every single pop (not once per flush, and not
/// once per poll_once() call - either would still leave a window where a
/// long-running inner loop pops several commands' worth of items, including
/// a brand-new one, before ever re-checking the control queue), and stamps
/// each newly-registered Subscription with `last_sequence_` - the sequence
/// of the last item already popped per shard, at that exact moment. A
/// subscription then only ever delivers events whose sequence is strictly
/// later than its own stamp (Subscription::wants()). Because every event of
/// one command shares that command's sequence number, this threshold can
/// never admit some of a command's events while excluding others: either the
/// whole command's sequence is later than the stamp (nothing of it had been
/// popped yet when this subscription registered - delivered in full) or it
/// is not (some of it had already been popped - none of it is delivered,
/// including the part not yet popped). The corollary this also has to prove
/// is the one a production incident (task 011, CI) found missing from an
/// earlier version of this file: a command submitted strictly after
/// subscribe() returns must be delivered in full. It is, by the same
/// argument - draining right before every pop means the registration is
/// never stale by more than "the one pop immediately following it", and
/// program order on the calling thread (subscribe() returns, only then is
/// the command even submitted) guarantees that pop cannot be any part of
/// that later command.
class Publisher {
public:
    /// `doorbell` is rung by each shard once per released batch (task 007),
    /// so this constructor does not own it - Engine owns one Doorbell shared
    /// by every shard and the publisher, to avoid the construction-order
    /// cycle a Publisher-owned doorbell would create (shards need a
    /// reference to it, but Publisher needs the shards' egress queues).
    Publisher(std::vector<EgressQueue*> sources,
              concurrency::Doorbell& doorbell,
              std::size_t max_batch = 1024);

    /// Must be called before the publisher thread starts.
    void add_subscriber(EventSubscriber& subscriber);

    /// Registers a new Subscription and returns the consumer's handle to it.
    /// Thread-safe from any thread: registration is handed to the publisher
    /// thread through a bounded control queue, so subscriptions_ itself is
    /// still touched only by the publisher (ADR-0003's single-writer rule).
    /// See the class comment for exactly when registration takes effect.
    /// If the publisher thread has already returned from run() (or does so
    /// while this call is retrying a full control queue), this returns an
    /// already-closed Subscription (Subscription::close(): overflowed(),
    /// poll() always 0) instead of retrying forever - nothing will ever
    /// drain the control queue again. This covers subscribe() calls made
    /// after Engine::stop() has returned (a real happens-before edge via
    /// the join inside it) soundly; a subscribe() racing a concurrent,
    /// in-progress stop() is a best-effort case, not a linearizable
    /// guarantee - document any caller that needs the latter.
    std::shared_ptr<Subscription> subscribe(SubscriptionFilter filter, std::size_t capacity);

    /// Thread body. Returns after `stop` is requested and all sources are
    /// drained. Precondition: the shard threads have already exited. Before
    /// returning, closes (Subscription::close()) every Subscription still
    /// registered or still sitting in the control queue, so a consumer
    /// waiting only on a subscription's on_ready() hook does not hang past
    /// shutdown.
    void run(const std::stop_token& stop);

private:
    std::size_t poll_once();
    void flush_events();
    void drain_control();
    void prune_subscriptions();
    void deliver_to(Subscription& subscription);

    std::vector<EgressQueue*> sources_;
    concurrency::Doorbell& doorbell_;
    std::vector<EventSubscriber*> subscribers_;
    std::vector<std::shared_ptr<Subscription>> subscriptions_;
    // Sized well above any expected concurrent-subscribe burst; subscribe()
    // retries (yielding) on the rare chance it is ever full, the same
    // back-pressure shape as Engine::broadcast()'s ingress retry.
    SubscriptionControlQueue control_{1024};
    std::vector<PublishedEvent> pending_;
    // last_sequence_[shard] = the sequence of the last item popped for that
    // shard so far; see the class comment. Sized to sources_.size() in the
    // constructor and never resized afterwards. Publisher-thread-only.
    std::vector<domain::SequenceNumber> last_sequence_;
    std::size_t max_batch_;
    // Set just before run() returns, after closing every remaining
    // subscription; see subscribe()'s doc.
    std::atomic<bool> stopped_{false};
};

}  // namespace lockstep::app
