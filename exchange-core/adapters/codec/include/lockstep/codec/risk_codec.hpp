#pragma once

#include <array>

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
/// `event` must carry a domain::Trade; the shard id, sequence and timestamp
/// come from the PublishedEvent envelope.
[[nodiscard]] std::array<v1::ExecutionReport, 2> encode_execution_reports(
    const app::PublishedEvent& event, const domain::Trade& trade);

}  // namespace lockstep::codec
