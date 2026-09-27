#pragma once

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

#include "lockstep/app/messages.hpp"
#include "lockstep/domain/commands.hpp"

namespace lockstep::app {

/// Why a command could not even be queued. These never reach the journal:
/// nothing about the exchange state changed.
enum class SubmitError : std::uint8_t {
    Overloaded,         ///< the owning shard's ingress queue is full (back-pressure)
    UnknownInstrument,  ///< no shard owns the instrument
    NotRoutable,        ///< risk command passed to submit(); use broadcast()
    ShuttingDown,
};

[[nodiscard]] constexpr std::string_view to_string(SubmitError error) noexcept {
    switch (error) {
        case SubmitError::Overloaded:
            return "overloaded";
        case SubmitError::UnknownInstrument:
            return "unknown instrument";
        case SubmitError::NotRoutable:
            return "command is not routable to a single shard";
        case SubmitError::ShuttingDown:
            return "shutting down";
    }
    std::unreachable();
}

/// Inbound port: how driving adapters (gRPC services, the risk client) hand
/// commands to the application core. Thread-safe; never blocks on the shard.
class CommandIngress {
public:
    CommandIngress() = default;
    CommandIngress(const CommandIngress&) = delete;
    CommandIngress& operator=(const CommandIngress&) = delete;
    CommandIngress(CommandIngress&&) = delete;
    CommandIngress& operator=(CommandIngress&&) = delete;
    virtual ~CommandIngress() = default;

    /// Routes an order command to the shard owning its instrument. On success
    /// `completion` is invoked exactly once, later, on the publisher thread.
    /// On error it is destroyed without being invoked and the caller must
    /// answer its client itself.
    [[nodiscard]] virtual std::expected<void, SubmitError> submit(domain::Command command,
                                                                  Completion completion) = 0;

    /// Delivers a risk command to every shard (each journals and applies it).
    /// Waits for queue space rather than failing with Overloaded: risk commands
    /// are rare and must not be dropped. Fails only when shutting down.
    [[nodiscard]] virtual std::expected<void, SubmitError> broadcast(domain::Command command) = 0;
};

}  // namespace lockstep::app
