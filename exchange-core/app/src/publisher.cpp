#include "lockstep/app/publisher.hpp"

#include <algorithm>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace lockstep::app {

Publisher::Publisher(std::vector<EgressQueue*> sources,
                     concurrency::Doorbell& doorbell,
                     std::size_t max_batch)
    : sources_{std::move(sources)}, doorbell_{doorbell}, max_batch_{max_batch} {
    pending_.reserve(max_batch_);
}

void Publisher::add_subscriber(EventSubscriber& subscriber) {
    subscribers_.push_back(&subscriber);
}

std::shared_ptr<Subscription> Publisher::subscribe(SubscriptionFilter filter,
                                                   std::size_t capacity) {
    auto subscription = std::make_shared<Subscription>(std::move(filter), capacity);
    std::shared_ptr<Subscription> for_control = subscription;
    // try_push leaves for_control untouched on failure (ConcurrentQueue's
    // contract), so retrying with it is safe - same shape as Engine::submit's
    // retry loops.
    while (!control_.try_push(std::move(for_control))) {  // NOLINT(bugprone-use-after-move)
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
}

std::size_t Publisher::poll_once() {
    // Applied before touching any source, so a Subscription registered
    // through subscribe() is in subscriptions_ before this iteration can
    // deliver anything - the atomicity acceptance criterion 4 needs: a
    // subscription never sees a command's events that were already being
    // flushed when it was registered, only ones produced from here on.
    apply_control();
    std::size_t processed = 0;
    for (EgressQueue* source : sources_) {
        for (std::size_t n = 0; n < max_batch_; ++n) {
            std::optional<OutboundItem> item = source->try_pop();
            if (!item) {
                break;
            }
            ++processed;
            std::visit(
                [this]<typename T>(T& payload) {
                    if constexpr (std::is_same_v<T, PublishedEvent>) {
                        pending_.push_back(payload);
                    } else {
                        flush_events();  // a command's events go out before its reply
                        payload.completion(payload.reply);
                    }
                },
                *item);
        }
        flush_events();
    }
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

void Publisher::apply_control() {
    while (std::optional<std::shared_ptr<Subscription>> added = control_.try_pop()) {
        subscriptions_.push_back(std::move(*added));
    }
    // Drop subscriptions the consumer cancelled, or that this publisher
    // overflowed on a previous flush (ADR-0006's slow-consumer policy): free
    // the slot and stop paying to filter events for a dead subscriber.
    std::erase_if(subscriptions_, [](const std::shared_ptr<Subscription>& subscription) {
        return subscription->cancelled() || subscription->overflowed();
    });
}

void Publisher::deliver_to(Subscription& subscription) {
    if (subscription.cancelled() || subscription.overflowed()) {
        return;  // apply_control() drops it on the next poll_once() iteration
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
