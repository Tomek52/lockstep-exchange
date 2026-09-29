#include "lockstep/domain/shard_engine.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <variant>

#include "lockstep/domain/matching.hpp"
#include "lockstep/domain/validation.hpp"

namespace lockstep::domain {

ShardEngine::ShardEngine(const ShardConfig& config)
    : shard_{config.shard}, risk_{config.risk_link_policy} {
    for (const InstrumentSpec& spec : config.instruments) {
        books_.try_emplace(spec.id, spec);
    }
}

CommandResult ShardEngine::apply(const SequencedCommand& command, EventBuffer& out) {
    return std::visit([this, &out](const auto& cmd) { return on(cmd, out); }, command.command);
}

bool ShardEngine::owns(InstrumentId instrument) const noexcept {
    return books_.contains(instrument);
}

const OrderBook* ShardEngine::book(InstrumentId instrument) const noexcept {
    const auto it = books_.find(instrument);
    return it == books_.end() ? nullptr : &it->second;
}

OrderBook* ShardEngine::find_book(InstrumentId instrument) noexcept {
    const auto it = books_.find(instrument);
    return it == books_.end() ? nullptr : &it->second;
}

OrderId ShardEngine::next_order_id() noexcept {
    ++order_counter_;
    return OrderId{(std::uint64_t{shard_.value()} << order_id_shard_shift) | order_counter_};
}

bool ShardEngine::is_duplicate_client_order(TraderId trader,
                                            ClientOrderId client_order_id) const noexcept {
    // A boolean answer does not depend on visiting order (ADR-0004: nothing
    // here is influenced by *which* book answers true first), but books_ is
    // still walked in its ordinary deterministic flat_map order, for
    // consistency with the rest of the engine rather than out of necessity.
    return std::ranges::any_of(books_, [trader, client_order_id](const auto& entry) {
        return entry.second.has_resting_client_order(trader, client_order_id);
    });
}

CommandResult ShardEngine::on(const NewOrder& order, EventBuffer& out) {
    OrderBook* book = find_book(order.instrument);
    if (book == nullptr) {
        return std::unexpected(RejectReason::UnknownInstrument);
    }
    if (const auto ok = validate(order, book->spec()).and_then([&] {
            return risk_.check_new_order(order.trader);
        });
        !ok) {
        return std::unexpected(ok.error());
    }
    if (is_duplicate_client_order(order.trader, order.client_order_id)) {
        return std::unexpected(RejectReason::DuplicateClientOrderId);
    }

    const OrderId id = next_order_id();
    const OrderAccepted accepted{id,         order.trader, order.client_order_id, order.instrument,
                                 order.side, order.type,   order.price,           order.quantity};
    out.push(accepted);

    const Quantity remaining = match(*book, accepted, out);
    if (remaining > Quantity{0}) {
        if (order.type == OrderType::Limit && order.time_in_force == TimeInForce::Gtc) {
            book->rest(RestingOrder{id, order.trader, order.client_order_id, order.side,
                                    order.price, remaining},
                       out);
        } else {
            out.push(OrderCancelled{id, order.trader, order.instrument, remaining,
                                    CancelReason::ImmediateOrCancel});
        }
    }
    return CommandOutcome{id};
}

CommandResult ShardEngine::on(const CancelOrder& cancel, EventBuffer& out) {
    OrderBook* book = find_book(cancel.instrument);
    if (book == nullptr) {
        return std::unexpected(RejectReason::UnknownInstrument);
    }
    return book->cancel(cancel.order_id, cancel.trader, CancelReason::UserRequested, out)
        .transform([&](Quantity /*cancelled*/) { return CommandOutcome{cancel.order_id}; });
}

CommandResult ShardEngine::on(const ModifyOrder& modify, EventBuffer& out) {
    OrderBook* book = find_book(modify.instrument);
    if (book == nullptr) {
        return std::unexpected(RejectReason::UnknownInstrument);
    }
    // Mirrors on(NewOrder): validate reference-data limits, then the risk
    // gate, before looking at order-specific state (UnknownOrder /
    // NotOrderOwner below). The spec (task 004) only says ModifyOrder is
    // "also gated by check_new_order"; this ordering was chosen to match the
    // existing NewOrder precedent rather than invent a second one.
    if (const auto ok = validate(modify, book->spec()).and_then([&] {
            return risk_.check_new_order(modify.trader);
        });
        !ok) {
        return std::unexpected(ok.error());
    }
    const RestingOrder* resting = book->find(modify.order_id);
    if (resting == nullptr) {
        return std::unexpected(RejectReason::UnknownOrder);
    }
    if (resting->trader != modify.trader) {
        return std::unexpected(RejectReason::NotOrderOwner);
    }

    // Same price and a smaller (or unchanged) quantity: shrink in place and
    // keep the order's queue position (domain-model.md "Matching rules").
    if (modify.new_price == resting->price && modify.new_quantity <= resting->remaining) {
        out.push(OrderModified{modify.order_id, modify.trader, modify.instrument, modify.new_price,
                               modify.new_quantity, /*kept_priority=*/true});
        if (modify.new_quantity < resting->remaining) {
            book->reduce(modify.order_id, modify.new_quantity, out);
        }
        return CommandOutcome{modify.order_id};
    }

    // Price change, or a larger quantity: cancel/replace. The order loses its
    // queue position and re-enters as a new incoming order with the same
    // OrderId, so it may trade before whatever remains rests at the tail.
    out.push(OrderModified{modify.order_id, modify.trader, modify.instrument, modify.new_price,
                           modify.new_quantity, /*kept_priority=*/false});
    const std::optional<RestingOrder> taken = book->take(modify.order_id, out);
    // `resting` was found on this same book just above, and nothing between
    // then and here can remove it (single-writer, no reentrancy: ADR-0003),
    // so take() always succeeds.
    assert(taken.has_value());
    const OrderAccepted incoming{modify.order_id,   modify.trader,      taken->client_order_id,
                                 modify.instrument, taken->side,        OrderType::Limit,
                                 modify.new_price,  modify.new_quantity};
    const Quantity remaining = match(*book, incoming, out);
    if (remaining > Quantity{0}) {
        // Only GTC limit orders ever rest, so any order reachable here (via
        // book->find() above) started as one; the remainder always rests
        // rather than being IOC-cancelled.
        book->rest(RestingOrder{modify.order_id, modify.trader, taken->client_order_id, taken->side,
                                modify.new_price, remaining},
                   out);
    }
    return CommandOutcome{modify.order_id};
}

CommandResult ShardEngine::on(const BlockTrader& block, EventBuffer& out) {
    risk_.block(block.trader);
    // books_ is a flat_map, so this walks instruments in id order
    // (ADR-0004). A trader blocked earlier already had every resting order
    // cancelled, so a duplicate BlockTrader finds nothing left to cancel here
    // and this loop emits nothing: idempotency (ADR-0013) falls out of
    // cancel_if's own behaviour rather than needing a separate guard.
    for (auto&& [instrument, book_ref] : books_) {
        book_ref.cancel_if(
            [trader = block.trader](const RestingOrder& order) { return order.trader == trader; },
            CancelReason::TraderBlocked, out);
    }
    out.push(RiskCommandApplied{block.command_id});
    return CommandOutcome{};
}

CommandResult ShardEngine::on(const UnblockTrader& unblock, EventBuffer& out) {
    risk_.unblock(unblock.trader);
    out.push(RiskCommandApplied{unblock.command_id});
    return CommandOutcome{};
}

CommandResult ShardEngine::on(const KillSwitch& kill, EventBuffer& out) {
    // Unlike BlockTrader, cancel_if alone would not make a redundant engage
    // a no-op: with nothing left to cancel it would still emit
    // InstrumentStatusChanged for every book. The spec and ADR-0013 only
    // require idempotency for re-engaging an already-engaged kill switch;
    // treating a redundant disengage the same way is a deliberate extension
    // here, not something either mandates, because it avoids emitting a
    // false InstrumentStatusChanged{halted=false} transition when nothing
    // actually resumed. Both directions are gated on the state changing.
    if (risk_.halted() != kill.engaged) {
        risk_.set_kill_switch(kill.engaged);
        // books_ is a flat_map: instrument-id order, deterministic (ADR-0004).
        for (auto&& [instrument, book_ref] : books_) {
            if (kill.engaged) {
                book_ref.cancel_if([](const RestingOrder&) { return true; },
                                   CancelReason::KillSwitch, out);
            }
            out.push(InstrumentStatusChanged{instrument, kill.engaged});
        }
    }
    out.push(RiskCommandApplied{kill.command_id});
    return CommandOutcome{};
}

CommandResult ShardEngine::on(const RiskLinkStatus& status, EventBuffer& /*out*/) {
    risk_.set_link_connected(status.connected);
    return CommandOutcome{};
}

}  // namespace lockstep::domain
