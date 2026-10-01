#include "lockstep/app/publisher.hpp"

#include <stop_token>
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
    pending_.clear();
}

}  // namespace lockstep::app
