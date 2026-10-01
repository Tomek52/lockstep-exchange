#include "lockstep/app/publisher.hpp"

#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

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
    auto subscription = std::make_shared<Subscription>(std::move(filter), capacity);
    // stopped_ is set only after run() has closed every subscription it
    // still knows about; a true result here means nothing will ever drain
    // control_ again, so pushing to it would just orphan this subscription.
    if (stopped_.load(std::memory_order_acquire)) {
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
        if (stopped_.load(std::memory_order_acquire)) {
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
    return subscription;
}

void Publisher::run(const std::stop_token& stop) {
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
    stopped_.store(true, std::memory_order_release);
}

std::size_t Publisher::poll_once() {
    std::size_t processed = 0;
    for (EgressQueue* source : sources_) {
        for (std::size_t n = 0; n < max_batch_; ++n) {
            // Drained immediately before every pop - see the class comment
            // on why this exact placement (not once per poll_once() call,
            // not once per flush) is what makes registration race-free
            // against both a command that starts after it and one that is
            // already partway through being flushed when it happens.
            drain_control();
            std::optional<OutboundItem> item = source->try_pop();
            if (!item) {
                break;
            }
            ++processed;
            std::visit(
                [this]<typename T>(T& payload) {
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
        // Snapshot last_sequence_ as it stands right now - i.e. reflecting
        // every item popped strictly before this registration and nothing
        // popped after it, since drain_control() never runs *after* a pop
        // without running again *before* the next one. See the class
        // comment for why this is the property that makes both failure
        // modes (a dropped post-registration command, a partially-delivered
        // pre-registration one) impossible.
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

}  // namespace lockstep::app
