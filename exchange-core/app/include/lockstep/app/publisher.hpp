#pragma once

#include <cstddef>
#include <memory>
#include <stop_token>
#include <vector>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/ports/event_subscriber.hpp"
#include "lockstep/app/queues.hpp"
#include "lockstep/app/subscription.hpp"
#include "lockstep/concurrency/idle_strategy.hpp"

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
    std::shared_ptr<Subscription> subscribe(SubscriptionFilter filter, std::size_t capacity);

    /// Thread body. Returns after `stop` is requested and all sources are
    /// drained. Precondition: the shard threads have already exited.
    void run(const std::stop_token& stop);

private:
    std::size_t poll_once();
    void flush_events();
    void apply_control();
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
    std::size_t max_batch_;
};

}  // namespace lockstep::app
