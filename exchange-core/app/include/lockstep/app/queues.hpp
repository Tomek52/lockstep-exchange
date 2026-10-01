#pragma once

#include <memory>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/subscription.hpp"
#include "lockstep/concurrency/idle_strategy.hpp"
#include "lockstep/concurrency/mpsc_queue.hpp"
#include "lockstep/concurrency/queue_concepts.hpp"
#include "lockstep/concurrency/spsc_queue.hpp"

namespace lockstep::app {

// The single place where the runtime's queue and idle types are chosen.
//
// The concept checks below make each swap a one-line change that cannot pick
// a queue with the wrong producer policy.

using IngressQueue = concurrency::MpscQueue<InboundCommand>;
using EgressQueue = concurrency::SpscQueue<OutboundItem>;
// Subscribe requests (task 011): any thread may call Engine::subscribe(), but
// only the publisher thread may touch its subscriptions_ list (ADR-0003's
// single-writer rule extended to that list), so registration is handed off
// through this queue instead of locking the list.
using SubscriptionControlQueue = concurrency::MpscQueue<std::shared_ptr<Subscription>>;
// ParkingIdle (task 007): parks on a Doorbell instead of BackoffIdle's fixed
// sleep_for, so an idle thread uses no CPU and wakes as soon as a producer
// rings rather than up to one sleep period late.
using RuntimeIdle = concurrency::ParkingIdle;

static_assert(concurrency::MultiProducerQueue<IngressQueue>,
              "ingress has many producers: gRPC threads and the risk client");
static_assert(concurrency::SingleProducerQueue<EgressQueue>);
static_assert(concurrency::MultiProducerQueue<SubscriptionControlQueue>,
              "subscribe() may be called from any thread");
static_assert(concurrency::IdleStrategy<RuntimeIdle>);

}  // namespace lockstep::app
