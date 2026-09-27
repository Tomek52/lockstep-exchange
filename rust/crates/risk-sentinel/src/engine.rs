//! Pure risk decision logic: fills in, risk actions out. No I/O, no async.

use crate::limits::RiskLimits;
use crate::positions::{Lots, Side, Ticks};

/// One side of an execution, decoded from an `ExecutionReport`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fill {
    pub trader: u64,
    pub instrument: u32,
    pub side: Side,
    pub price: Ticks,
    pub quantity: Lots,
}

/// What the sentinel asks the exchange to do.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RiskAction {
    BlockTrader { trader: u64, reason: String },
    KillSwitch { engaged: bool, reason: String },
}

/// Evaluates every fill against the limits.
///
/// SKELETON STATUS: records nothing and never acts.
/// See docs/tasks/015-sentinel-positions-pnl.md.
#[derive(Debug, Default)]
pub struct RiskEngine {
    limits: RiskLimits,
}

impl RiskEngine {
    #[must_use]
    pub fn new(limits: RiskLimits) -> Self {
        Self { limits }
    }

    #[must_use]
    pub fn limits(&self) -> &RiskLimits {
        &self.limits
    }

    /// Applies a fill and returns the actions it triggers (usually none).
    pub fn on_fill(&mut self, _fill: &Fill) -> Vec<RiskAction> {
        // TODO(task-015): update the position book and compare against limits;
        // emit each action once (idempotent), not on every subsequent fill.
        Vec::new()
    }
}
