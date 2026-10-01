#include "lockstep/app/shard_runtime.hpp"

#include <stop_token>
#include <thread>
#include <utility>

#include "lockstep/app/fatal.hpp"

namespace lockstep::app {

ShardRuntime::ShardRuntime(const Config& config,
                           std::unique_ptr<Journal> journal,
                           Clock& clock,
                           concurrency::Doorbell& publisher_doorbell)
    : engine_{config.shard},
      journal_{std::move(journal)},
      clock_{clock},
      max_batch_{config.max_batch},
      ingress_{config.ingress_capacity},
      egress_{config.egress_capacity},
      publisher_doorbell_{publisher_doorbell} {
    staged_.reserve(max_batch_ * 4);
}

void ShardRuntime::run(const std::stop_token& stop) {
    RuntimeIdle idle{doorbell_, stop};
    // Wakes this thread if it is parked when stop is requested: request_stop()
    // by itself does not touch the doorbell, so a thread idling on empty
    // ingress would otherwise never re-check stop_requested() (ADR-0003's
    // shutdown protocol needs this thread to notice promptly).
    const std::stop_callback wake_on_stop{stop, [this] { doorbell_.ring(); }};
    while (!stop.stop_requested()) {
        idle.idle(poll_once());
        // relaxed: diagnostic only, see ShardStats's comment.
        parks_stat_.store(idle.parks(), std::memory_order_relaxed);
    }
    // Producers were stopped before us (Engine shutdown protocol), so draining
    // until empty loses nothing.
    while (poll_once() > 0) {
    }
}

ShardStats ShardRuntime::stats() const noexcept {
    // relaxed: see ShardStats's comment.
    return ShardStats{
        .commands = commands_stat_.load(std::memory_order_relaxed),
        .batches = batches_stat_.load(std::memory_order_relaxed),
        .max_batch = max_batch_stat_.load(std::memory_order_relaxed),
        .parks = parks_stat_.load(std::memory_order_relaxed),
    };
}

std::size_t ShardRuntime::poll_once() {
    std::size_t processed = 0;
    while (processed < max_batch_) {
        std::optional<InboundCommand> inbound = ingress_.try_pop();
        if (!inbound) {
            break;
        }
        process(std::move(*inbound));
        ++processed;
    }
    if (processed > 0) {
        if (const auto committed = journal_->commit(); !committed) {
            fatal(to_string(committed.error()));
        }
        release_staged();
        // relaxed: diagnostic only, see ShardStats's comment. Single writer
        // (this thread), so fetch_add/load+store need no stronger ordering.
        commands_stat_.fetch_add(processed, std::memory_order_relaxed);
        batches_stat_.fetch_add(1, std::memory_order_relaxed);
        if (processed > max_batch_stat_.load(std::memory_order_relaxed)) {
            max_batch_stat_.store(processed, std::memory_order_relaxed);
        }
    }
    return processed;
}

void ShardRuntime::process(InboundCommand inbound) {
    const domain::SequencedCommand command{domain::SequenceNumber{++sequence_}, clock_.now(),
                                           inbound.command};

    // Write-ahead: the command is journaled before it can have any effect.
    if (const auto appended = journal_->append(command); !appended) {
        fatal(to_string(appended.error()));
    }

    events_.clear();
    const domain::CommandResult result = engine_.apply(command, events_);

    for (const domain::Event& event : events_.events()) {
        staged_.emplace_back(PublishedEvent{shard(), command.sequence, command.timestamp, event});
    }
    if (inbound.completion) {
        staged_.emplace_back(
            ReplyTask{std::move(inbound.completion),
                      CommandReply{shard(), command.sequence, command.timestamp, result}});
    }
}

void ShardRuntime::release_staged() {
    for (OutboundItem& item : staged_) {
        // try_push leaves `item` untouched on failure, so retrying is safe.
        // A full egress means a parked publisher hasn't drained it yet,
        // and yielding alone never wakes a parked thread - only ring() does
        // (task 007). Without ringing here, a batch whose output exceeds
        // egress_capacity would spin this thread forever: the end-of-batch
        // ring below is unreachable until this loop returns.
        while (!egress_.try_push(std::move(item))) {  // NOLINT(bugprone-use-after-move)
            publisher_doorbell_.ring();
            std::this_thread::yield();
        }
    }
    staged_.clear();
    // Also ring after the whole batch is visible, in case the last item's
    // push succeeded without the loop above ever needing to ring - the
    // publisher must still learn about it.
    publisher_doorbell_.ring();
}

}  // namespace lockstep::app
