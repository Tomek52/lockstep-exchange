//! Risk limits. Values are in the same fixed-point units as positions.

use crate::positions::Notional;

/// Per-trader and global limits that trigger risk commands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RiskLimits {
    /// Absolute net position (lots) per trader and instrument before blocking.
    pub max_abs_position: i128,
    /// Loss (tick-lots, positive number) per trader before blocking.
    pub max_trader_loss: Notional,
    /// Aggregate loss across all traders that engages the kill switch.
    pub kill_switch_loss: Notional,
}

impl Default for RiskLimits {
    fn default() -> Self {
        Self {
            max_abs_position: 10_000,
            max_trader_loss: 1_000_000,
            kill_switch_loss: 10_000_000,
        }
    }
}
