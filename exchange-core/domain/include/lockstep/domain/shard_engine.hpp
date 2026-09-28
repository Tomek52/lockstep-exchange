#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lockstep/domain/commands.hpp"
#include "lockstep/domain/events.hpp"
#include "lockstep/domain/flat_map.hpp"
#include "lockstep/domain/order_book.hpp"
#include "lockstep/domain/reject_reason.hpp"
#include "lockstep/domain/risk_state.hpp"
#include "lockstep/domain/strong_int.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

struct ShardConfig {
    ShardId shard;
    std::vector<InstrumentSpec> instruments;
    RiskLinkPolicy risk_link_policy{RiskLinkPolicy::FailOpen};
};

/// Positive result of a command. `order_id` is the new id for NewOrder, the
/// affected order for Cancel/Modify, and zero for risk commands.
struct CommandOutcome {
    OrderId order_id;

    friend constexpr bool operator==(const CommandOutcome&, const CommandOutcome&) = default;
};

using CommandResult = std::expected<CommandOutcome, RejectReason>;

/// All domain state owned by one shard: the order books of its instruments and
/// its copy of the risk controls. The whole exchange is N independent
/// ShardEngines, each driven by exactly one thread (ADR-0003).
///
/// Determinism contract (ADR-0004): for a given ShardConfig, the sequence of
/// (CommandResult, events) produced by apply() is a pure function of the
/// sequence of SequencedCommands passed in. No clocks, randomness, I/O, or
/// iteration over hash containers may influence it.
class ShardEngine {
public:
    explicit ShardEngine(const ShardConfig& config);

    /// Applies one command, appending every resulting event to `out` (which the
    /// caller clears between commands).
    [[nodiscard]] CommandResult apply(const SequencedCommand& command, EventBuffer& out);

    [[nodiscard]] ShardId shard() const noexcept { return shard_; }
    [[nodiscard]] bool owns(InstrumentId instrument) const noexcept;
    [[nodiscard]] const OrderBook* book(InstrumentId instrument) const noexcept;
    [[nodiscard]] const RiskState& risk() const noexcept { return risk_; }

    /// Order ids embed the shard id in the top 16 bits so they are unique across
    /// shards without coordination; 48 bits of counter allow ~2.8e14 orders.
    static constexpr unsigned order_id_shard_shift = 48;

private:
    CommandResult on(const NewOrder& order, EventBuffer& out);
    CommandResult on(const CancelOrder& cancel, EventBuffer& out);
    CommandResult on(const ModifyOrder& modify, EventBuffer& out);
    CommandResult on(const BlockTrader& block, EventBuffer& out);
    CommandResult on(const UnblockTrader& unblock, EventBuffer& out);
    CommandResult on(const KillSwitch& kill, EventBuffer& out);
    CommandResult on(const RiskLinkStatus& status, EventBuffer& out);

    [[nodiscard]] OrderBook* find_book(InstrumentId instrument) noexcept;
    [[nodiscard]] OrderId next_order_id() noexcept;

    // True if (trader, client_order_id) is still resting on some book in the
    // shard (RejectReason::DuplicateClientOrderId, task 003). Self-heals
    // client_orders_: a stale entry (its order since filled or cancelled) is
    // erased here rather than kept in sync at every removal site, since this
    // lookup is the only place that needs the answer.
    [[nodiscard]] bool is_duplicate_client_order(TraderId trader,
                                                 ClientOrderId client_order_id) noexcept;
    // Records that (trader, client_order_id) now names the resting order
    // `id` on `instrument`, for later is_duplicate_client_order() lookups.
    void track_client_order(TraderId trader,
                            ClientOrderId client_order_id,
                            InstrumentId instrument,
                            OrderId id);

    ShardId shard_;
    flat_map<InstrumentId, OrderBook> books_;
    RiskState risk_;
    std::uint64_t order_counter_{0};

    struct ClientOrderKey {
        TraderId trader;
        ClientOrderId client_order_id;
        friend constexpr bool operator==(const ClientOrderKey&, const ClientOrderKey&) = default;
    };
    struct ClientOrderKeyHash {
        [[nodiscard]] std::size_t operator()(const ClientOrderKey& key) const noexcept {
            // Combined via xor/shift, as std::hash has no tuple overload; order
            // of the two hashes does not matter, only that they mix (ADR-0004:
            // this is a lookup key, never iterated, so hash quality only
            // affects performance, not determinism).
            return StrongIntHash{}(key.trader) ^ (StrongIntHash{}(key.client_order_id) << 1);
        }
    };

    // Index from (trader, client_order_id) to the resting order it currently
    // names, so on(NewOrder) can reject a reused id in O(1) without iterating
    // any book (task 003). Hash order is unspecified, so, like
    // OrderBook::slot_by_id_, this map is only ever looked up, never iterated.
    std::unordered_map<ClientOrderKey, std::pair<InstrumentId, OrderId>, ClientOrderKeyHash>
        client_orders_;
};

}  // namespace lockstep::domain
