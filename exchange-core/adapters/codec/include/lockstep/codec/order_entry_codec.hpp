#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include "lockstep/app/messages.hpp"
#include "lockstep/app/ports/command_ingress.hpp"
#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/reject_reason.hpp"
#include <lockstep/v1/order_entry.pb.h>
#include <lockstep/v1/risk.pb.h>

namespace lockstep::codec {

// Translation between wire messages (lockstep.v1) and domain commands.
//
// Decoding is total: every byte sequence protobuf accepts maps either to a
// valid domain command or to a DecodeError - never to an out-of-range enum
// value inside the domain. This is the boundary the fuzzer hammers
// (fuzz/order_entry_decode_fuzzer.cpp).

enum class DecodeError : std::uint8_t {
    InvalidSide,
    InvalidOrderType,
    InvalidTimeInForce,
    MissingRiskAction,
};

[[nodiscard]] std::string_view to_string(DecodeError error) noexcept;

[[nodiscard]] std::expected<domain::NewOrder, DecodeError> decode(
    const v1::SubmitOrderRequest& request) noexcept;
[[nodiscard]] std::expected<domain::CancelOrder, DecodeError> decode(
    const v1::CancelOrderRequest& request) noexcept;
[[nodiscard]] std::expected<domain::ModifyOrder, DecodeError> decode(
    const v1::ModifyOrderRequest& request) noexcept;
[[nodiscard]] std::expected<domain::Command, DecodeError> decode(
    const v1::RiskCommand& command) noexcept;

/// Fills `ack` from the shard's reply.
void encode(const app::CommandReply& reply, v1::CommandAck& ack);

/// Fills `ack` for a command that never reached a shard (shard_sequence = 0).
void encode_rejection(v1::RejectReason reason, std::string_view detail, v1::CommandAck& ack);

[[nodiscard]] v1::RejectReason to_proto(domain::RejectReason reason) noexcept;
[[nodiscard]] v1::RejectReason to_proto(DecodeError error) noexcept;
[[nodiscard]] v1::RejectReason to_proto(app::SubmitError error) noexcept;

}  // namespace lockstep::codec
