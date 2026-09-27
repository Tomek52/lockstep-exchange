#pragma once

#include <expected>

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
///
/// SKELETON STATUS: accepts every order and ignores state changes.
/// See docs/tasks/004-risk-controls-in-domain.md.
class RiskState {
public:
    explicit RiskState(RiskLinkPolicy policy = RiskLinkPolicy::FailOpen) noexcept
        : policy_{policy} {}

    /// Pre-trade gate for NewOrder / ModifyOrder.
    [[nodiscard]] std::expected<void, RejectReason> check_new_order(TraderId trader) const noexcept;

    void block(TraderId trader);
    void unblock(TraderId trader);
    void set_kill_switch(bool engaged) noexcept;
    void set_link_connected(bool connected) noexcept;

    [[nodiscard]] bool is_blocked(TraderId trader) const noexcept;
    [[nodiscard]] bool halted() const noexcept;
    [[nodiscard]] RiskLinkPolicy policy() const noexcept { return policy_; }

private:
    RiskLinkPolicy policy_;
};

}  // namespace lockstep::domain
