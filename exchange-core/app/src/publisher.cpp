#include "lockstep/app/publisher.hpp"

#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "lockstep/app/fatal.hpp"

namespace lockstep::app {

Publisher::Publisher(std::vector<EgressQueue*> sources,
                     concurrency::Doorbell& doorbell,
                     std::size_t max_batch)
    : sources_{std::move(sources)},
      doorbell_{doorbell},
      last_sequence_(sources_.size(), domain::SequenceNumber{0}),
      max_batch_{max_batch} {
    pending_.reserve(max_batch_);
}

void Publisher::add_subscriber(EventSubscriber& subscriber) {
    subscribers_.push_back(&subscriber);
}

std::shared_ptr<Subscription> Publisher::subscribe(SubscriptionFilter filter,
                                                   std::size_t capacity) {
    // Fail fast instead of spinning forever: if run() has started, nothing
    // but the publisher thread itself ever drives the registration
    // handshake this call waits on below, so calling it from that thread
    // (e.g. from inside a completion or an on_ready() hook) can never make
    // progress (see the doc above). publisher_thread_id_ is still its
    // default-constructed "no thread" value before run() ever runs, which
    // never compares equal to a real thread's id, so this is a no-op for
    // every pre-start call.
    if (std::this_thread::get_id() == publisher_thread_id_.load(std::memory_order_relaxed)) {
        fatal(
            "Publisher::subscribe() called from the publisher thread itself "
            "(e.g. from a completion or an on_ready() hook) - this would "
            "deadlock, since nothing else drives the registration handshake "
            "it waits on");
    }
    auto subscription = std::make_shared<Subscription>(std::move(filter), capacity);
    {
        const std::lock_guard<std::mutex> lock(pre_start_mutex_);
        if (!started_) {
            if (pre_start_closed_) {
                // Engine::stop() ran with no start() ever called (see
                // close_before_start()) - nothing will ever drain this, so
                // hand back an already-closed Subscription rather than one
                // that looks registered but can never receive anything.
                subscription->close();
                return subscription;
            }
            // No publisher thread exists yet to send this through control_
            // and wait on - register directly instead (see subscribe()'s
            // doc for why this is race-free against a concurrent start()).
            // last_sequence_ is still all zeros at this point (nothing has
            // run yet to advance it), which is the correct cutoff: this
            // subscription sees every event from here on.
            subscription->set_start_sequence(last_sequence_);
            subscriptions_.push_back(subscription);
            return subscription;
        }
    }
    // seq_cst: paired with the seq_cst store in run() - see stopped_'s doc.
    // stopped_ is set only after run() has closed every subscription it
    // still knows about; a true result here means nothing will ever drain
    // control_ again, so pushing to it would just orphan this subscription.
    if (stopped_.load(std::memory_order_seq_cst)) {
        subscription->close();
        return subscription;
    }
    std::shared_ptr<Subscription> for_control = subscription;
    // try_push leaves for_control untouched on failure (ConcurrentQueue's
    // contract), so retrying with it is safe - same shape as Engine::submit's
    // retry loops. Re-checks stopped_ each time, not just before the first
    // attempt: the publisher can stop while this loop is retrying a full
    // queue, and without this check it would spin forever (task 011 review).
    while (!control_.try_push(std::move(for_control))) {  // NOLINT(bugprone-use-after-move)
        if (stopped_.load(std::memory_order_seq_cst)) {
            subscription->close();
            return subscription;
        }
        std::this_thread::yield();
    }
    // Ring only after the push is visible, per Doorbell's happens-before
    // contract (idle_strategy.hpp) - see Engine::submit's identical comment:
    // a ring before the push could wake a waiter that sees nothing and parks
    // again with no further ring left to wake it.
    doorbell_.ring();
    // Block until the publisher thread has actually drained and stamped
    // this subscription (see is_registered()'s doc), rather than returning
    // as soon as the push above is merely visible. This is the fix for a
    // concurrency review finding: control_ is a Dmitry Vyukov MPSC queue
    // (concurrency/mpsc_queue.hpp), and its documented stall property means
    // a push being visible in real time does not mean try_pop() can see it
    // yet - a different, concurrently-stalled producer earlier in the queue
    // can make drain_control() report "empty" even though this push has
    // already landed. Returning on visibility alone could let a caller
    // submit a command before this subscription was actually registered,
    // with nothing to stop that command's events from being accounted for
    // (last_sequence_ advanced past them) before the still-unregistered
    // subscription is finally drained - dropping it entirely despite having
    // been submitted strictly after subscribe() returned. Waiting for the
    // publisher's own acknowledgement sidesteps the question of exactly
    // when a push becomes poppable: whatever last_sequence_ is at the
    // moment drain_control() actually processes this subscription is
    // correct by construction (set_start_sequence()'s doc), for any drain
    // placement.
    while (!subscription->is_registered()) {
        if (stopped_.load(std::memory_order_seq_cst)) {
            // Covers both: this push never reached drain_control() before
            // shutdown's own pass (close() unblocks us directly), and the
            // rarer case where it did and run() already closed it (the
            // is_registered() check above would then already be true, so
            // this branch is not reached) - either way, safe and idempotent.
            subscription->close();
            break;
        }
        std::this_thread::yield();
    }
    return subscription;
}

void Publisher::run(const std::stop_token& stop) {
    // relaxed: see the member's doc - only this thread's own later reads of
    // its own earlier write need to see it, which same-thread program order
    // already guarantees.
    publisher_thread_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    {
        // Closes the pre-start registration window (subscribe()'s doc): any
        // subscribe() call already holding pre_start_mutex_, or that
        // acquires it before this one, finishes registering directly into
        // subscriptions_ before this thread is allowed to proceed past this
        // point; any call that acquires it afterwards sees started_ already
        // true and takes the control-queue path instead. Either way,
        // subscriptions_ never has two writers.
        const std::lock_guard<std::mutex> lock(pre_start_mutex_);
        started_ = true;
    }
    RuntimeIdle idle{doorbell_, stop};
    // See ShardRuntime::run's identical callback: wakes this thread if it is
    // parked when stop is requested.
    const std::stop_callback wake_on_stop{stop, [this] { doorbell_.ring(); }};
    while (!stop.stop_requested()) {
        idle.idle(poll_once());
    }
    // Shards have exited; whatever is still queued is the last output.
    while (poll_once() > 0) {
    }
    // One more drain to catch a subscribe() that raced this shutdown (see
    // subscribe()'s doc); then close everything this publisher still knows
    // about, so a consumer waiting on on_ready() never hangs past shutdown,
    // and only then flip stopped_ - subscribe() must not see it true while a
    // registration it just pushed could still be sitting un-drained here.
    drain_control();
    for (const std::shared_ptr<Subscription>& subscription : subscriptions_) {
        subscription->close();
    }
    // Reset before this thread actually terminates: std::thread::id values
    // are only guaranteed unique among *currently running* threads (the
    // standard allows a terminated thread's id to be reused by a later
    // one), so leaving the real id in place here could make some future,
    // unrelated thread alias it and get wrongly fatal()'d by subscribe()'s
    // guard (found by this round's own regression test reusing a thread
    // that happened to recycle the just-joined publisher thread's id).
    // relaxed: see the member's doc.
    publisher_thread_id_.store(std::thread::id{}, std::memory_order_relaxed);
    stopped_.store(true, std::memory_order_seq_cst);  // seq_cst: see subscribe()'s checks
}

std::size_t Publisher::poll_once() {
    std::size_t processed = 0;
    for (EgressQueue* source : sources_) {
        // Once per source, before its inner pop loop: the safety net that
        // registers a subscription even on a source with nothing to pop at
        // all, so one never waits forever behind a quiet shard (subscribe()
        // blocks on exactly this - see its doc). Correctness of what a
        // registered subscription then receives does not depend on this
        // call's exact placement relative to any particular pop below (see
        // the class comment): wherever last_sequence_ happens to stand when
        // a subscription is actually drained is the correct cutoff for it,
        // by construction.
        drain_control();
        for (std::size_t n = 0; n < max_batch_; ++n) {
            std::optional<OutboundItem> item = source->try_pop();
            if (!item) {
                break;
            }
            ++processed;
            std::visit(
                [this]<typename T>(T& payload) {
                    // Indexing last_sequence_ by the event's own ShardId,
                    // not this loop's position in sources_: relies on
                    // Engine building sources_ in shard-id order (shard k's
                    // egress at index k, engine.cpp's egress_queues()), so
                    // every ShardId this publisher ever sees is in bounds.
                    if constexpr (std::is_same_v<T, PublishedEvent>) {
                        last_sequence_[payload.shard.value()] = payload.sequence;
                        pending_.push_back(payload);
                    } else {
                        last_sequence_[payload.reply.shard.value()] = payload.reply.sequence;
                        flush_events();  // a command's events go out before its reply
                        payload.completion(payload.reply);
                    }
                },
                *item);
        }
        flush_events();
    }
    prune_subscriptions();
    return processed;
}

void Publisher::flush_events() {
    if (pending_.empty()) {
        return;
    }
    for (EventSubscriber* subscriber : subscribers_) {
        subscriber->on_events(pending_);
    }
    for (const std::shared_ptr<Subscription>& subscription : subscriptions_) {
        deliver_to(*subscription);
    }
    pending_.clear();
}

void Publisher::drain_control() {
    while (std::optional<std::shared_ptr<Subscription>> added = control_.try_pop()) {
        // Snapshot last_sequence_ as it stands right now, and mark the
        // subscription registered (set_start_sequence()'s doc) - the signal
        // subscribe() is waiting on before it returns this same object to
        // its caller. That handshake, not this call's placement, is what
        // guarantees a command submitted after subscribe() returns is
        // delivered in full (see Publisher::subscribe's doc); this snapshot
        // only has to be internally consistent - never including an item
        // not yet accounted for in last_sequence_ - which holds here
        // because nothing updates last_sequence_ except this same thread,
        // and never concurrently with this loop.
        (*added)->set_start_sequence(last_sequence_);
        subscriptions_.push_back(std::move(*added));
    }
}

void Publisher::prune_subscriptions() {
    // Drop subscriptions the consumer cancelled, or that this publisher
    // overflowed or closed (ADR-0006's slow-consumer policy, or shutdown):
    // free the slot and stop paying to filter events for a dead subscriber.
    // Once per poll_once() call is enough - "on its next iteration" per the
    // spec, not latency-sensitive the way registration ordering is.
    std::erase_if(subscriptions_, [](const std::shared_ptr<Subscription>& subscription) {
        return subscription->cancelled() || subscription->overflowed();
    });
}

void Publisher::deliver_to(Subscription& subscription) {
    if (subscription.cancelled() || subscription.overflowed()) {
        return;  // prune_subscriptions() drops it on the next poll_once() iteration
    }
    bool touched = false;
    for (const PublishedEvent& event : pending_) {
        if (!subscription.wants(event)) {
            continue;
        }
        touched = true;
        if (!subscription.deliver(event)) {
            break;  // ring just overflowed: stop handing it more this flush
        }
    }
    if (touched) {
        subscription.notify_ready();
    }
}

void Publisher::close_before_start() {
    const std::lock_guard<std::mutex> lock(pre_start_mutex_);
    if (started_ || pre_start_closed_) {
        // started_: run() did start (concurrently with, or before, this
        // call) and owns shutdown from here - its own close-everything pass
        // covers whatever is in subscriptions_. pre_start_closed_: an
        // earlier call already did this (Engine::stop() is idempotent).
        return;
    }
    pre_start_closed_ = true;
    // Only ever reached via the pre-start direct-registration path
    // (subscribe()'s doc), so every entry here was added under this same
    // mutex and nothing else is touching subscriptions_ right now.
    for (const std::shared_ptr<Subscription>& subscription : subscriptions_) {
        subscription->close();
    }
}

}  // namespace lockstep::app
