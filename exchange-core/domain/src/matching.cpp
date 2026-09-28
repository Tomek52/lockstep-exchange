#include "lockstep/domain/matching.hpp"

#include <algorithm>

namespace lockstep::domain {

namespace {

// The price-crossing rule (docs/architecture/domain-model.md "Matching
// rules"): a market order has no price limit; a limit order may trade only
// within its own limit.
[[nodiscard]] bool crosses(const OrderAccepted& incoming, Price maker_price) noexcept {
    if (incoming.type == OrderType::Market) {
        return true;
    }
    return incoming.side == Side::Buy ? maker_price <= incoming.price
                                      : maker_price >= incoming.price;
}

}  // namespace

Quantity match(OrderBook& book, const OrderAccepted& incoming, EventBuffer& out) {
    const Side maker_side = opposite(incoming.side);
    Quantity remaining = incoming.quantity;

    while (remaining > Quantity{0}) {
        const RestingOrder* maker = book.front(maker_side);
        if (maker == nullptr || !crosses(incoming, maker->price)) {
            break;
        }

        // Copied out before reduce_front(), which may remove the maker's slot
        // and invalidate this pointer.
        const OrderId maker_id = maker->id;
        const TraderId maker_trader = maker->trader;
        const Price trade_price = maker->price;
        const Quantity trade_quantity = std::min(remaining, maker->remaining);

        out.push(Trade{incoming.instrument, trade_price, trade_quantity, incoming.side, maker_id,
                       maker_trader, incoming.order_id, incoming.trader});
        book.reduce_front(maker_side, trade_quantity, out);

        remaining -= trade_quantity;
    }
    return remaining;
}

}  // namespace lockstep::domain
