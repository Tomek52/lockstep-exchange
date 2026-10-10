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

impl RiskLimits {
    /// Checks that no limit is negative. The fields are plain `i128`, and a
    /// negative loss limit would block a flat trader (PnL 0 is below it).
    pub fn validate(&self) -> Result<(), String> {
        for (name, value) in [
            ("max-abs-position", self.max_abs_position),
            ("max-trader-loss", self.max_trader_loss),
            ("kill-switch-loss", self.kill_switch_loss),
        ] {
            if value < 0 {
                return Err(format!("--{name} must not be negative, got {value}"));
            }
        }
        Ok(())
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_valid() {
        assert_eq!(RiskLimits::default().validate(), Ok(()));
    }

    #[test]
    fn zero_is_allowed() {
        let limits = RiskLimits {
            max_abs_position: 0,
            max_trader_loss: 0,
            kill_switch_loss: 0,
        };
        assert_eq!(limits.validate(), Ok(()));
    }

    #[test]
    fn each_negative_limit_is_rejected_by_name() {
        let ok = RiskLimits::default();
        let cases = [
            (
                RiskLimits {
                    max_abs_position: -1,
                    ..ok
                },
                "--max-abs-position",
            ),
            (
                RiskLimits {
                    max_trader_loss: -1,
                    ..ok
                },
                "--max-trader-loss",
            ),
            (
                RiskLimits {
                    kill_switch_loss: i128::MIN,
                    ..ok
                },
                "--kill-switch-loss",
            ),
        ];
        for (limits, flag) in cases {
            let error = limits.validate().expect_err("negative limit");
            assert!(error.contains(flag), "{error}");
        }
    }
}
