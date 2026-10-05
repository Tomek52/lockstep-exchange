#pragma once

#include <concepts>
#include <ranges>
#include <utility>
#include <vector>

#include "lockstep/app/messages.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/events.hpp"
#include "lockstep/domain/shard_engine.hpp"

namespace lockstep::app {

/// Everything a shard emitted, in order. Two runs are equivalent iff their
/// ReplayOutputs compare equal (plus equal book snapshots).
struct ReplayOutput {
    std::vector<PublishedEvent> events;
    std::vector<CommandReply> replies;

    friend bool operator==(const ReplayOutput&, const ReplayOutput&) = default;
};

/// Re-applies journaled commands to a fresh engine, single-threaded and without
/// clocks or queues. Accepts any input range of SequencedCommand, including the
/// lazy std::generator the file journal reader produces (task 009).
template <std::ranges::input_range Commands>
    requires std::convertible_to<std::ranges::range_reference_t<Commands>,
                                 const domain::SequencedCommand&>
[[nodiscard]] ReplayOutput replay(domain::ShardEngine& engine, Commands&& commands) {
    ReplayOutput output;
    domain::EventBuffer buffer;
    for (const domain::SequencedCommand& command : std::forward<Commands>(commands)) {
        buffer.clear();
        const domain::CommandResult result = engine.apply(command, buffer);
        for (const domain::Event& event : buffer.events()) {
            output.events.push_back(
                PublishedEvent{engine.shard(), command.sequence, command.timestamp, event});
        }
        output.replies.push_back(
            CommandReply{engine.shard(), command.sequence, command.timestamp, result});
    }
    return output;
}

}  // namespace lockstep::app
