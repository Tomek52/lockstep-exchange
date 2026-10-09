#pragma once

// Shared fixtures for app-level and determinism tests.

#include <chrono>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "lockstep/app/engine.hpp"
#include "lockstep/app/ports/event_subscriber.hpp"
#include "lockstep/journal/memory_journal.hpp"

namespace lockstep::test {

/// Records everything the publisher delivers. Written on the publisher thread,
/// read by the test only after Engine::stop() joined that thread.
class RecordingSubscriber final : public app::EventSubscriber {
public:
    void on_events(std::span<const app::PublishedEvent> events) override {
        events_.insert(events_.end(), events.begin(), events.end());
    }
    [[nodiscard]] const std::vector<app::PublishedEvent>& events() const { return events_; }

private:
    std::vector<app::PublishedEvent> events_;
};

/// JournalFactory that hands out MemoryJournals and keeps pointers to them so a
/// test can inspect each shard's journal after the engine stopped.
class MemoryJournals {
public:
    app::JournalFactory factory() {
        return [this](domain::ShardId /*shard*/) {
            auto journal = std::make_unique<journal::MemoryJournal>();
            journals_.push_back(journal.get());
            return journal;
        };
    }
    [[nodiscard]] const journal::MemoryJournal& of(domain::ShardId shard) const {
        return *journals_.at(shard.value());
    }

private:
    std::vector<journal::MemoryJournal*> journals_;
};

/// Completion that fulfils a future; lets a test wait for one reply.
inline std::pair<app::Completion, std::future<app::CommandReply>> reply_future() {
    auto promise = std::make_shared<std::promise<app::CommandReply>>();
    auto future = promise->get_future();
    return {[promise](const app::CommandReply& reply) noexcept { promise->set_value(reply); },
            std::move(future)};
}

inline constexpr auto reply_timeout = std::chrono::seconds{10};

/// Polls `pred` until it returns true or `timeout` elapses. Returns the last
/// value of `pred`. Never waits for the full timeout when the condition holds
/// early. Use this instead of a fixed sleep before asserting positive thread
/// state: a fixed sleep is a property of the machine, not of the code.
template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate pred,
                              std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return pred();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

}  // namespace lockstep::test
