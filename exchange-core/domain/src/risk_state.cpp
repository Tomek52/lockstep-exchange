#include "lockstep/domain/risk_state.hpp"

namespace lockstep::domain {

// SKELETON: every method below is a deliberate no-op so that the walking
// skeleton can wire risk commands end to end. The real behaviour, and the tests
// that pin it down, are specified in docs/tasks/004-risk-controls-in-domain.md.

std::expected<void, RejectReason> RiskState::check_new_order(TraderId /*trader*/) const noexcept {
    return {};  // TODO(task-004): blocked trader, kill switch, fail-closed link policy
}

void RiskState::block(TraderId /*trader*/) {}  // TODO(task-004)

void RiskState::unblock(TraderId /*trader*/) {}  // TODO(task-004)

void RiskState::set_kill_switch(bool /*engaged*/) noexcept {}  // TODO(task-004)

void RiskState::set_link_connected(bool /*connected*/) noexcept {}  // TODO(task-004)

bool RiskState::is_blocked(TraderId /*trader*/) const noexcept {
    return false;  // TODO(task-004)
}

bool RiskState::halted() const noexcept {
    return false;  // TODO(task-004)
}

}  // namespace lockstep::domain
