#pragma once

#include <span>

#include "lockstep/app/messages.hpp"

namespace lockstep::app {

/// Outbound port: receives published events (market data fan-out, the risk
/// client's execution reports). Called on the publisher thread with events in
/// per-shard sequence order. Implementations must not block: buffer and hand
/// off to their own I/O machinery (ADR-0006 slow-consumer policy).
class EventSubscriber {
public:
    EventSubscriber() = default;
    EventSubscriber(const EventSubscriber&) = delete;
    EventSubscriber& operator=(const EventSubscriber&) = delete;
    EventSubscriber(EventSubscriber&&) = delete;
    EventSubscriber& operator=(EventSubscriber&&) = delete;
    virtual ~EventSubscriber() = default;

    virtual void on_events(std::span<const PublishedEvent> events) = 0;
};

}  // namespace lockstep::app
