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
/// Registering a Subscription without splitting a command: `release_staged()`
/// (ShardRuntime) pushes one command's events to egress one try_push() at a
/// time, not as a single atomic unit, so this thread's try_pop() can observe
/// "nothing more right now" partway through a command, and later see the
/// rest once the shard catches up. A Subscription registered in that gap
/// must see either *all* of that command's events or *none* of them, never
/// just the tail. Every PublishedEvent already carries its command's
/// per-shard `sequence` (events.hpp/messages.hpp), so this is solved without
/// buffering further than `pending_` already does: every newly-drained
/// Subscription is stamped with `last_sequence_` - the sequence of the last
/// item already popped per shard - and only ever delivers events whose
/// sequence is strictly later (Subscription::wants()). Because every event
/// of one command shares that command's sequence number, this threshold can
/// never admit some of a command's events while excluding others: either the
/// whole command's sequence is later than the stamp (nothing of it had been
/// popped yet when this subscription registered - delivered in full) or it
/// is not (some of it had already been popped - none of it is delivered,
/// including the part not yet popped). This half of the argument holds for
/// *any* placement of `drain_control()` - it only needs the snapshot it
/// takes to be internally consistent, which it always is (publisher-thread-
/// only state, see `last_sequence_`'s doc).
///
/// The other half - a command submitted strictly *after* `subscribe()`
/// returns is always delivered in full - does not come from timing
/// `drain_control()`'s placement at all (an earlier version of this file
/// tried that, and a concurrency review found a case it still missed:
/// `control_` is a multi-producer queue with the stall property documented
/// in `concurrency/mpsc_queue.hpp` - a concurrently-stalled *other* producer
/// earlier in the queue can make this subscription's own, already-landed
/// push unreachable to `try_pop()` for an unbounded time, no matter when
/// `drain_control()` runs). Instead, `subscribe()` itself blocks until the
/// publisher has actually drained and stamped the subscription
/// (`Subscription::is_registered()`) before returning it to its caller.
/// Since nothing the caller does afterwards (e.g. submitting a command) can
/// happen before that acknowledgement, and that command's events cannot be
/// popped before the command itself is produced, `last_sequence_` cannot
/// yet include any part of it at the moment this subscription registers -
/// whatever `drain_control()` call actually processes it, whenever that is.
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

    /// Registers a new Subscription and returns the consumer's handle to it,
    /// blocking (a spin, never a park - typically well under one publisher
    /// iteration) until the publisher has acknowledged the registration (see
    /// the class comment for why that handshake, not just handing the
    /// request off, is required). Thread-safe from any thread *except* the
    /// publisher thread itself - calling this from inside a completion or an
    /// on_ready() hook would deadlock, since nothing else ever drives
    /// drain_control() to acknowledge it.
    ///
    /// Before the publisher thread has ever started running(), nothing
    /// drains the control queue at all, so a call made then blocks until
    /// run() does (and, if the queue also happens to be at capacity already,
    /// until run() drains far enough to make room - mirrors every other
    /// producer-side queue in this runtime, e.g. Engine::submit() before
    /// start(), and is not specific to subscribe()).
    ///
    /// If the publisher thread has already returned from run(), or does so
    /// while this call is still waiting, this returns an already-closed
    /// Subscription (Subscription::close(): overflowed(), poll() always 0,
    /// is_registered() true) instead of waiting forever - nothing will ever
    /// drain the control queue again once the publisher has stopped. This
    /// covers subscribe() calls made after Engine::stop() has returned (a
    /// real happens-before edge via the join inside it) soundly; a
    /// subscribe() racing a concurrent, in-progress stop() is covered too,
    /// by the same wait loop that is already checking stopped_ - run()'s
    /// shutdown closes every subscription it drained before flipping
    /// stopped_, so a subscription this call is waiting on is always either
    /// already registered (and therefore already closed, if shutdown reached
    /// it) by the time stopped_ becomes visible, or never will be, in which
    /// case this call closes it itself.
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
    // subscription; see subscribe()'s doc. seq_cst on both this store and
    // subscribe()'s loads: the two also need to agree on the relative order
    // of this store and run()'s own drain_control() a few lines earlier, not
    // just synchronise with each other, and acquire/release does not give
    // that - only a single total order (seq_cst) does.
    std::atomic<bool> stopped_{false};
};

}  // namespace lockstep::app
