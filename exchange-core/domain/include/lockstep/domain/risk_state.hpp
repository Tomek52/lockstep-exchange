#pragma once

#include <expected>
#include <vector>

#include "lockstep/domain/reject_reason.hpp"
#include "lockstep/domain/types.hpp"

namespace lockstep::domain {

/// What to do with new orders while the risk sentinel is unreachable (ADR-0013).
enum class RiskLinkPolicy : std::uint8_t {
    FailOpen,    ///< keep trading; risk is post-trade (default)
    FailClosed,  ///< reject new orders with RiskUnavailable until the link is back
};

/// Per-shard risk controls driven by risk-sentinel commands: blocked traders,
/// the kill switch, and link status. Pure state; the commands that change it
/// arrive through the journal like any order (ADR-0004).
class RiskState {
public:
    explicit RiskState(RiskLinkPolicy policy = RiskLinkPolicy::FailOpen) noexcept
        : policy_{policy} {}

    /// Pre-trade gate for NewOrder / ModifyOrder. Checks, in order: halted
    /// (TradingHalted), trader blocked (TraderBlocked), then, only under
    /// FailClosed, the link being down (RiskUnavailable). See
    /// docs/tasks/004-risk-controls-in-domain.md for why this order.
    [[nodiscard]] std::expected<void, RejectReason> check_new_order(TraderId trader) const noexcept;

    void block(TraderId trader);
    void unblock(TraderId trader);
    void set_kill_switch(bool engaged) noexcept;
    void set_link_connected(bool connected) noexcept;

    [[nodiscard]] bool is_blocked(TraderId trader) const noexcept;
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    [[nodiscard]] RiskLinkPolicy policy() const noexcept { return policy_; }

private:
    RiskLinkPolicy policy_;
    bool halted_{false};
    // The link starts down (ADR-0013): under FailClosed, orders are rejected
    // until the shard applies the first RiskLinkStatus{true}, so replay
    // reproduces exactly which early orders were refused.
    bool link_connected_{false};
    // Sorted, never iterated (ADR-0004): membership is a binary search.
    // ShardEngine cancels a blocked trader's resting orders by asking each
    // OrderBook via cancel_if, not by walking this set.
    std::vector<TraderId> blocked_;
};

}  // namespace lockstep::domain
