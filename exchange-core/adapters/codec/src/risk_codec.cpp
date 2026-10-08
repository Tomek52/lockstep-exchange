#include "lockstep/codec/risk_codec.hpp"

#include <utility>

namespace lockstep::codec {

namespace {

[[nodiscard]] v1::Side to_proto(domain::Side side) noexcept {
    switch (side) {
        case domain::Side::Buy:
            return v1::SIDE_BUY;
        case domain::Side::Sell:
            return v1::SIDE_SELL;
    }
    std::unreachable();  // exhaustive; -Wswitch catches new enumerators
}

/// Fills one report side of a trade. `is_maker` distinguishes the resting
/// order (true) from the aggressor (false).
void fill_report(const app::PublishedEvent& event,
                 const domain::Trade& trade,
                 domain::TraderId trader,
                 domain::OrderId order,
                 domain::Side side,
                 bool is_maker,
                 v1::ExecutionReport& report) {
    report.set_shard_id(event.shard.value());
    report.set_shard_sequence(event.sequence.value());
    report.set_timestamp_ns(event.timestamp.value());
    report.set_trader_id(trader.value());
    report.set_instrument_id(trade.instrument.value());
    report.set_order_id(order.value());
    report.set_side(to_proto(side));
    report.set_price_ticks(trade.price.value());
    report.set_quantity(trade.quantity.value());
    report.set_is_maker(is_maker);
}

}  // namespace

std::array<v1::ExecutionReport, 2> encode_execution_reports(const app::PublishedEvent& event,
                                                            const domain::Trade& trade) {
    std::array<v1::ExecutionReport, 2> reports;
    // Taker: the aggressor, by definition on `aggressor_side`, took liquidity.
    fill_report(event, trade, trade.taker_trader, trade.taker_order, trade.aggressor_side,
                /*is_maker=*/false, reports[0]);
    // Maker: the resting order on the opposite side provided liquidity.
    fill_report(event, trade, trade.maker_trader, trade.maker_order,
                domain::opposite(trade.aggressor_side), /*is_maker=*/true, reports[1]);
    return reports;
}

}  // namespace lockstep::codec
