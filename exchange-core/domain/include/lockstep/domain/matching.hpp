#pragma once

#include "lockstep/domain/events.hpp"
#include "lockstep/domain/order_book.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

/// Matches `incoming` against the opposite side of `book` with price-time
/// priority (docs/architecture/domain-model.md "Matching rules"): best price
/// first, oldest order first within a price. A limit order matches only
/// resting orders that cross its price; a market order matches any price.
///
/// Appends one `Trade` (aggressor = `incoming.side`, trade price = the
/// resting/maker order's price) followed by the `BookLevelChanged` that
/// `OrderBook::reduce_front` emits for the maker's level, per fill.
///
/// Returns the quantity left unmatched. The caller (`ShardEngine::on`) rests
/// it on `incoming.side` for a GTC limit, or cancels it otherwise; `match`
/// itself never rests or cancels.
[[nodiscard]] Quantity match(OrderBook& book, const OrderAccepted& incoming, EventBuffer& out);

}  // namespace lockstep::domain
