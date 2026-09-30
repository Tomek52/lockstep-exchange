#include "lockstep/domain/risk_state.hpp"

#include <algorithm>

namespace lockstep::domain {

std::expected<void, RejectReason> RiskState::check_new_order(TraderId trader) const noexcept {
    if (halted_) {
        return std::unexpected(RejectReason::TradingHalted);
    }
    if (is_blocked(trader)) {
        return std::unexpected(RejectReason::TraderBlocked);
    }
    if (policy_ == RiskLinkPolicy::FailClosed && !link_connected_) {
        return std::unexpected(RejectReason::RiskUnavailable);
    }
    return {};
}

void RiskState::block(TraderId trader) {
    const auto it = std::ranges::lower_bound(blocked_, trader);
    if (it == blocked_.end() || *it != trader) {
        blocked_.insert(it, trader);
    }
}

void RiskState::unblock(TraderId trader) {
    const auto it = std::ranges::lower_bound(blocked_, trader);
    if (it != blocked_.end() && *it == trader) {
        blocked_.erase(it);
    }
}

void RiskState::set_kill_switch(bool engaged) noexcept {
    halted_ = engaged;
}

void RiskState::set_link_connected(bool connected) noexcept {
    link_connected_ = connected;
}

bool RiskState::is_blocked(TraderId trader) const noexcept {
    return std::ranges::binary_search(blocked_, trader);
}

}  // namespace lockstep::domain
