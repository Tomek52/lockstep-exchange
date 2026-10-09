#include "lockstep/codec/market_data_codec.hpp"

#include <type_traits>
#include <utility>
#include <variant>

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

void fill(const domain::Trade& trade, v1::Trade& out) {
    out.set_instrument_id(trade.instrument.value());
    out.set_price_ticks(trade.price.value());
    out.set_quantity(trade.quantity.value());
    out.set_aggressor_side(to_proto(trade.aggressor_side));
}

void fill(const domain::BookLevelChanged& change, v1::BookLevelUpdate& out) {
    out.set_instrument_id(change.instrument.value());
    out.set_side(to_proto(change.side));
    out.set_price_ticks(change.price.value());
    out.set_quantity(change.quantity.value());
}

void fill(const domain::InstrumentStatusChanged& status, v1::InstrumentStatus& out) {
    out.set_instrument_id(status.instrument.value());
    out.set_halted(status.halted);
}

}  // namespace

bool encode(const app::PublishedEvent& event, v1::SubscribeResponse& out) {
    // The visitor below classifies every alternative: three public kinds and
    // four that are not (OrderAccepted, OrderCancelled, OrderModified,
    // RiskCommandApplied). A new event kind must be classified deliberately,
    // not fall into the "not public" branch by default.
    static_assert(std::variant_size_v<domain::Event> == 7,
                  "classify the new event kind in market_data_codec.cpp");
    out.Clear();
    const bool is_public = std::visit(
        [&out](const auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, domain::Trade>) {
                fill(payload, *out.mutable_trade());
                return true;
            } else if constexpr (std::is_same_v<Payload, domain::BookLevelChanged>) {
                fill(payload, *out.mutable_book_level_update());
                return true;
            } else if constexpr (std::is_same_v<Payload, domain::InstrumentStatusChanged>) {
                fill(payload, *out.mutable_instrument_status());
                return true;
            } else {
                return false;  // order lifecycle and risk acks are not public data
            }
        },
        event.event);
    if (!is_public) {
        return false;
    }
    out.set_shard_id(event.shard.value());
    out.set_shard_sequence(event.sequence.value());
    out.set_timestamp_ns(event.timestamp.value());
    return true;
}

}  // namespace lockstep::codec
