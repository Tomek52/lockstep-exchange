#pragma once

#include "lockstep/app/messages.hpp"
#include "lockstep/concurrency/idle_strategy.hpp"
#include "lockstep/concurrency/mutex_queue.hpp"
#include "lockstep/concurrency/queue_concepts.hpp"

namespace lockstep::app {

// The single place where the runtime's queue and idle types are chosen.
//
// SKELETON: both queues are the mutex-based placeholder. Task 006 switches
// IngressQueue to concurrency::MpscQueue, task 005 switches EgressQueue to
// concurrency::SpscQueue, task 007 revisits the idle strategy. The concept
// checks below make each swap a one-line change that cannot pick a queue with
// the wrong producer policy.

using IngressQueue = concurrency::MutexQueue<InboundCommand>;
using EgressQueue = concurrency::MutexQueue<OutboundItem>;
using RuntimeIdle = concurrency::BackoffIdle;

static_assert(concurrency::MultiProducerQueue<IngressQueue>,
              "ingress has many producers: gRPC threads and the risk client");
static_assert(concurrency::SingleProducerQueue<EgressQueue>);
static_assert(concurrency::IdleStrategy<RuntimeIdle>);

}  // namespace lockstep::app
