#pragma once

#include <cstddef>
#include <stop_token>
#include <vector>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/ports/event_subscriber.hpp"
#include "lockstep/app/queues.hpp"
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
/// SKELETON STATUS: subscribers are registered before start and called
/// synchronously. Task 011 adds per-subscriber bounded buffers and the
/// slow-consumer policy.
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

    /// Thread body. Returns after `stop` is requested and all sources are
    /// drained. Precondition: the shard threads have already exited.
    void run(const std::stop_token& stop);

private:
    std::size_t poll_once();
    void flush_events();

    std::vector<EgressQueue*> sources_;
    concurrency::Doorbell& doorbell_;
    std::vector<EventSubscriber*> subscribers_;
    std::vector<PublishedEvent> pending_;
    std::size_t max_batch_;
};

}  // namespace lockstep::app
