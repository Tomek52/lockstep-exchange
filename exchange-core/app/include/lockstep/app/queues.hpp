#pragma once

#include "lockstep/app/messages.hpp"
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
// ParkingIdle (task 007): parks on a Doorbell instead of BackoffIdle's fixed
// sleep_for, so an idle thread uses no CPU and wakes as soon as a producer
// rings rather than up to one sleep period late.
using RuntimeIdle = concurrency::ParkingIdle;

static_assert(concurrency::MultiProducerQueue<IngressQueue>,
              "ingress has many producers: gRPC threads and the risk client");
static_assert(concurrency::SingleProducerQueue<EgressQueue>);
static_assert(concurrency::IdleStrategy<RuntimeIdle>);

}  // namespace lockstep::app
