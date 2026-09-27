#pragma once

#include <functional>
#include <variant>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/events.hpp"
#include "lockstep/domain/shard_engine.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::app {

/// The outcome of one command, stamped with where it sits in its shard's
/// journal. Replaying the journal reproduces every CommandReply exactly.
struct CommandReply {
    domain::ShardId shard;
    domain::SequenceNumber sequence;
    domain::Timestamp timestamp;
    domain::CommandResult result;

    friend bool operator==(const CommandReply&, const CommandReply&) = default;
};

/// Callback that delivers a CommandReply to whoever submitted the command
/// (typically a gRPC reactor). Invoked exactly once, on the publisher thread,
/// after the command has been journaled and its events published. Must not
/// block. move_only_function: completions own move-only state (reactor handles).
using Completion = std::move_only_function<void(const CommandReply&) noexcept>;

/// Ingress item: gRPC / risk threads -> shard.
struct InboundCommand {
    domain::Command command;
    Completion completion;  ///< empty for broadcast risk commands
};

/// A domain event with its provenance. Trivially copyable (see DomainEvent).
struct PublishedEvent {
    domain::ShardId shard;
    domain::SequenceNumber sequence;
    domain::Timestamp timestamp;
    domain::Event event;

    friend bool operator==(const PublishedEvent&, const PublishedEvent&) = default;
};

struct ReplyTask {
    Completion completion;
    CommandReply reply;
};

/// Egress item: shard -> publisher.
using OutboundItem = std::variant<PublishedEvent, ReplyTask>;

}  // namespace lockstep::app
