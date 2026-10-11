#pragma once

#include <array>
#include <cstdint>

#include "lockstep/app/messages.hpp"
#include "lockstep/domain/events.hpp"
#include <lockstep/v1/risk.pb.h>

namespace lockstep::codec {

// Translation from domain Trade events to the risk loop's wire messages
// (lockstep.v1, ADR-0013). The inbound direction (RiskCommand -> domain
// Command) lives in order_entry_codec.hpp; both share the proto library.

/// A Trade produces exactly two ExecutionReports: one for the taker (the
/// aggressor, which removed liquidity) and one for the maker (which was
/// resting on the book). Both carry the event's provenance (shard id, shard
/// sequence, timestamp) so the sentinel can dedupe after a reconnect
/// (ADR-0013).
///
/// All events of one command share its shard sequence, and a command that
/// sweeps several resting orders emits several Trades. `trade_index` is the
/// zero-based position of this Trade among the Trades of that command; with
/// the shard id, the sequence and `is_maker` it identifies a report
/// (ADR-0022). The caller counts, because the encoding is stateless.
///
/// `event` must carry a domain::Trade; the shard id, sequence and timestamp
/// come from the PublishedEvent envelope.
[[nodiscard]] std::array<v1::ExecutionReport, 2> encode_execution_reports(
    const app::PublishedEvent& event, const domain::Trade& trade, std::uint32_t trade_index);

}  // namespace lockstep::codec
