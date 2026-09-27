#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace lockstep::concurrency {

/// Bounded MPMC queue guarded by a mutex.
///
/// Deliberately simple and obviously correct. It has two jobs:
///  1. carry the walking skeleton until the lock-free queues land (tasks 005/006);
///  2. afterwards, serve as the reference model ("oracle") in differential tests
///     of SpscQueue and MpscQueue.
/// It is not used on the hot path once those tasks are done (see
/// lockstep/app/queues.hpp, the single place where queue types are chosen).
template <typename T>
class MutexQueue {
public:
    using value_type = T;
    static constexpr bool multi_producer = true;
    static constexpr bool multi_consumer = true;

    explicit MutexQueue(std::size_t capacity) : capacity_{capacity} {}

    [[nodiscard]] bool try_push(T&& value) {
        const std::scoped_lock lock{mutex_};
        if (items_.size() >= capacity_) {
            return false;  // `value` untouched, caller may retry
        }
        items_.push_back(std::move(value));
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        const std::scoped_lock lock{mutex_};
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> out{std::move(items_.front())};
        items_.pop_front();
        return out;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<T> items_;
};

}  // namespace lockstep::concurrency
