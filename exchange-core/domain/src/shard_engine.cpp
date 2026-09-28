#include "lockstep/domain/shard_engine.hpp"

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

CommandResult ShardEngine::on(const ModifyOrder& modify, EventBuffer& /*out*/) {
    const OrderBook* book = find_book(modify.instrument);
    if (book == nullptr) {
        return std::unexpected(RejectReason::UnknownInstrument);
    }
    if (const auto ok = validate(modify, book->spec()); !ok) {
        return std::unexpected(ok.error());
    }
    const RestingOrder* resting = book->find(modify.order_id);
    if (resting == nullptr) {
        return std::unexpected(RejectReason::UnknownOrder);
    }
    if (resting->trader != modify.trader) {
        return std::unexpected(RejectReason::NotOrderOwner);
    }
    // TODO(task-003): cancel/replace with priority rules. Reachable now that
    // task 002 rests orders; until task 003 lands, a modify by the order's
    // owner is refused with UnknownOrder, a placeholder reason rather than a
    // literally correct one.
    return std::unexpected(RejectReason::UnknownOrder);
}

CommandResult ShardEngine::on(const BlockTrader& block, EventBuffer& out) {
    risk_.block(block.trader);
    // TODO(task-004): cancel the trader's resting orders (CancelReason::TraderBlocked).
    out.push(RiskCommandApplied{block.command_id});
    return CommandOutcome{};
}

CommandResult ShardEngine::on(const UnblockTrader& unblock, EventBuffer& out) {
    risk_.unblock(unblock.trader);
    out.push(RiskCommandApplied{unblock.command_id});
    return CommandOutcome{};
}

CommandResult ShardEngine::on(const KillSwitch& kill, EventBuffer& out) {
    risk_.set_kill_switch(kill.engaged);
    // TODO(task-004): emit InstrumentStatusChanged per book (iterate books_ in
    // key order - deterministic) and cancel resting orders when engaging.
    out.push(RiskCommandApplied{kill.command_id});
    return CommandOutcome{};
}

CommandResult ShardEngine::on(const RiskLinkStatus& status, EventBuffer& /*out*/) {
    risk_.set_link_connected(status.connected);
    return CommandOutcome{};
}

}  // namespace lockstep::domain
