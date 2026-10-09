#pragma once

#include "lockstep/app/messages.hpp"
#include <lockstep/v1/market_data.pb.h>

namespace lockstep::codec {

// Translation from published domain events to the public market data stream
// (lockstep.v1.MarketDataService, ADR-0006).

/// Maps a public event (Trade, BookLevelChanged, InstrumentStatusChanged) to
/// one SubscribeResponse, stamped with the event's shard id, shard sequence
/// and timestamp. `out` is cleared first, so it can be reused between calls.
///
/// Returns false, leaving `out` cleared, for events that are not public market
/// data (order lifecycle events and risk acknowledgements). Public messages
/// never carry trader or order ids: Trade is mapped without maker/taker
/// identities.
[[nodiscard]] bool encode(const app::PublishedEvent& event, v1::SubscribeResponse& out);

}  // namespace lockstep::codec
