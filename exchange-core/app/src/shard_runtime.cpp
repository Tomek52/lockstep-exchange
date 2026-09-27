#include "lockstep/app/shard_runtime.hpp"

#include <thread>
#include <utility>

#include "lockstep/app/fatal.hpp"

namespace lockstep::app {

ShardRuntime::ShardRuntime(const Config& config, std::unique_ptr<Journal> journal, Clock& clock)
    : engine_{config.shard},
      journal_{std::move(journal)},
      clock_{clock},
      max_batch_{config.max_batch},
      ingress_{config.ingress_capacity},
      egress_{config.egress_capacity} {
    staged_.reserve(max_batch_ * 4);
}

void ShardRuntime::run(const std::stop_token& stop) {
    RuntimeIdle idle;
    while (!stop.stop_requested()) {
        idle.idle(poll_once());
    }
    // Producers were stopped before us (Engine shutdown protocol), so draining
    // until empty loses nothing.
    while (poll_once() > 0) {
    }
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
        // The publisher always drains, so this back-pressure is bounded.
        while (!egress_.try_push(std::move(item))) {  // NOLINT(bugprone-use-after-move)
            std::this_thread::yield();
        }
    }
    staged_.clear();
}

}  // namespace lockstep::app
