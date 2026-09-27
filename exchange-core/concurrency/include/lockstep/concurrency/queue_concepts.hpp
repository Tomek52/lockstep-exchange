#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>

namespace lockstep::concurrency {

/// A bounded, non-blocking queue.
///
/// Contract shared by every implementation (checked by the typed contract tests
/// in tests/concurrency/queue_contract_test.cpp):
///  * try_push(T&&) returns false when full and then leaves the argument
///    untouched (it is moved from only on success) - callers can retry;
///  * try_pop() returns std::nullopt when empty;
///  * FIFO per producer;
///  * never blocks, never allocates after construction.
template <typename Q>
concept ConcurrentQueue = requires(Q& queue, const Q& const_queue, typename Q::value_type&& value) {
    typename Q::value_type;
    { queue.try_push(std::move(value)) } -> std::same_as<bool>;
    { queue.try_pop() } -> std::same_as<std::optional<typename Q::value_type>>;
    { const_queue.capacity() } -> std::convertible_to<std::size_t>;
    { Q::multi_producer } -> std::convertible_to<bool>;
    { Q::multi_consumer } -> std::convertible_to<bool>;
};

/// Queue policy: safe for concurrent try_push from any number of threads.
/// Required for shard ingress (gRPC threads + risk client -> shard).
template <typename Q>
concept MultiProducerQueue = ConcurrentQueue<Q> && Q::multi_producer;

/// Queue policy: usable with exactly one producer and one consumer thread.
/// Every ConcurrentQueue qualifies; the concept exists to document intent at
/// the use site (shard -> publisher egress).
template <typename Q>
concept SingleProducerQueue = ConcurrentQueue<Q>;

}  // namespace lockstep::concurrency
