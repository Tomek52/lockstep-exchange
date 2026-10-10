//! Outcome counters, latency histogram, and the two output formats.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::time::Duration;

use hdrhistogram::Histogram;
use lockstep_proto::v1::RejectReason;
use serde_json::{Value, json};

/// Counters and latencies of one worker, or the merge of several.
#[derive(Debug)]
pub(crate) struct Stats {
    pub(crate) accepted: u64,
    /// Rejections by reason name, e.g. `REJECT_REASON_TRADER_BLOCKED`.
    pub(crate) rejected: BTreeMap<String, u64>,
    /// Operations abandoned after exhausting back-pressure retries.
    pub(crate) resource_exhausted: u64,
    /// Individual `RESOURCE_EXHAUSTED` responses that were retried.
    pub(crate) retries: u64,
    /// Transport failures and malformed responses.
    pub(crate) errors: u64,
    /// Round trip of the final attempt of each answered operation, in ns.
    latencies: Histogram<u64>,
}

impl Stats {
    pub(crate) fn new() -> Self {
        Self {
            accepted: 0,
            rejected: BTreeMap::new(),
            resource_exhausted: 0,
            retries: 0,
            errors: 0,
            latencies: Histogram::new(3).expect("3 significant figures is a valid precision"),
        }
    }

    pub(crate) fn record_latency(&mut self, latency: Duration) {
        let ns = u64::try_from(latency.as_nanos()).unwrap_or(u64::MAX);
        // `record` grows the histogram on demand; `saturating_record` would
        // clamp to the current range instead, so it is only the fallback.
        if self.latencies.record(ns).is_err() {
            self.latencies.saturating_record(ns);
        }
    }

    pub(crate) fn record_rejection(&mut self, reason: i32) {
        let name = RejectReason::try_from(reason).map_or_else(
            |_| format!("UNKNOWN_{reason}"),
            |r| r.as_str_name().to_owned(),
        );
        *self.rejected.entry(name).or_default() += 1;
    }

    pub(crate) fn merge(&mut self, other: &Stats) {
        self.accepted += other.accepted;
        for (reason, count) in &other.rejected {
            *self.rejected.entry(reason.clone()).or_default() += count;
        }
        self.resource_exhausted += other.resource_exhausted;
        self.retries += other.retries;
        self.errors += other.errors;
        self.latencies
            .add(&other.latencies)
            .expect("histograms auto-resize, so adding cannot overflow the range");
    }

    pub(crate) fn rejected_total(&self) -> u64 {
        self.rejected.values().sum()
    }

    /// Operations that got a final answer or were given up on.
    pub(crate) fn operations(&self) -> u64 {
        self.accepted + self.rejected_total() + self.resource_exhausted + self.errors
    }

    fn percentile_us(&self, quantile: f64) -> f64 {
        #[expect(clippy::cast_precision_loss)]
        let ns = self.latencies.value_at_quantile(quantile) as f64;
        ns / 1_000.0
    }

    fn max_us(&self) -> f64 {
        #[expect(clippy::cast_precision_loss)]
        let ns = self.latencies.max() as f64;
        ns / 1_000.0
    }

    fn ops_per_sec(&self, elapsed: Duration) -> f64 {
        #[expect(clippy::cast_precision_loss)]
        let ops = self.operations() as f64;
        let secs = elapsed.as_secs_f64();
        if secs > 0.0 { ops / secs } else { 0.0 }
    }
}

/// Extra numbers only some scenarios produce.
#[derive(Debug, Default, Clone, Copy)]
pub(crate) struct BreachReport {
    /// A `TRADER_BLOCKED` rejection was received.
    pub(crate) blocked: bool,
    /// Time from the first accepted aggressive order (the first fill, see
    /// `scenario::Breach`) to the first block; `None` if either is missing.
    pub(crate) first_fill_to_block: Option<Duration>,
}

pub(crate) fn to_json(
    scenario: &str,
    stats: &Stats,
    elapsed: Duration,
    breach: Option<BreachReport>,
) -> Value {
    let mut value = json!({
        "scenario": scenario,
        "elapsed_secs": elapsed.as_secs_f64(),
        "operations": stats.operations(),
        "ops_per_sec": stats.ops_per_sec(elapsed),
        "accepted": stats.accepted,
        "rejected": stats.rejected_total(),
        "rejected_by_reason": stats.rejected,
        "resource_exhausted": stats.resource_exhausted,
        "retries": stats.retries,
        "errors": stats.errors,
        "latency_us": {
            "p50": stats.percentile_us(0.50),
            "p90": stats.percentile_us(0.90),
            "p99": stats.percentile_us(0.99),
            "p99.9": stats.percentile_us(0.999),
            "max": stats.max_us(),
        },
    });
    if let Some(breach) = breach {
        value["blocked"] = json!(breach.blocked);
        value["first_fill_to_block_us"] =
            json!(breach.first_fill_to_block.map(|d| d.as_secs_f64() * 1e6));
    }
    value
}

pub(crate) fn to_text(
    scenario: &str,
    stats: &Stats,
    elapsed: Duration,
    breach: Option<BreachReport>,
) -> String {
    let mut out = String::new();
    // Writing to a String cannot fail.
    let _ = writeln!(
        out,
        "scenario={scenario} operations={} elapsed={:.2}s achieved={:.0} ops/s",
        stats.operations(),
        elapsed.as_secs_f64(),
        stats.ops_per_sec(elapsed),
    );
    let _ = writeln!(
        out,
        "accepted={} rejected={} resource_exhausted={} (retries={}) errors={}",
        stats.accepted,
        stats.rejected_total(),
        stats.resource_exhausted,
        stats.retries,
        stats.errors,
    );
    for (reason, count) in &stats.rejected {
        let _ = writeln!(out, "  rejected {reason}: {count}");
    }
    let _ = writeln!(
        out,
        "latency us: p50={:.0} p90={:.0} p99={:.0} p99.9={:.0} max={:.0}",
        stats.percentile_us(0.50),
        stats.percentile_us(0.90),
        stats.percentile_us(0.99),
        stats.percentile_us(0.999),
        stats.max_us(),
    );
    if let Some(breach) = breach {
        match (breach.blocked, breach.first_fill_to_block) {
            (true, Some(d)) => {
                let _ = writeln!(
                    out,
                    "first fill to TRADER_BLOCKED: {:.0} us",
                    d.as_secs_f64() * 1e6
                );
            }
            (true, None) => {
                let _ = writeln!(out, "TRADER_BLOCKED received before any fill");
            }
            (false, _) => {
                let _ = writeln!(out, "trader was never blocked");
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Stats {
        let mut stats = Stats::new();
        stats.accepted = 3;
        stats.record_rejection(RejectReason::TraderBlocked as i32);
        stats.record_rejection(RejectReason::TraderBlocked as i32);
        stats.record_rejection(999);
        stats.resource_exhausted = 1;
        stats.retries = 7;
        for us in [100, 200, 300, 400] {
            stats.record_latency(Duration::from_micros(us));
        }
        stats
    }

    #[test]
    fn counts_rejections_by_reason_and_keeps_back_pressure_apart() {
        let stats = sample();
        assert_eq!(stats.rejected["REJECT_REASON_TRADER_BLOCKED"], 2);
        assert_eq!(stats.rejected["UNKNOWN_999"], 1);
        assert_eq!(stats.rejected_total(), 3);
        assert_eq!(stats.operations(), 3 + 3 + 1);
    }

    #[test]
    fn merge_adds_counters_and_latencies() {
        let mut total = sample();
        total.merge(&sample());
        assert_eq!(total.accepted, 6);
        assert_eq!(total.rejected["REJECT_REASON_TRADER_BLOCKED"], 4);
        assert_eq!(total.retries, 14);
        assert!(
            total.max_us() >= 399.0,
            "max={} p99={}",
            total.max_us(),
            total.percentile_us(0.99)
        );
    }

    #[test]
    fn json_has_the_documented_fields() {
        let value = to_json("random", &sample(), Duration::from_secs(2), None);
        assert_eq!(value["accepted"], 3);
        assert_eq!(value["rejected"], 3);
        assert_eq!(value["resource_exhausted"], 1);
        assert_eq!(
            value["rejected_by_reason"]["REJECT_REASON_TRADER_BLOCKED"],
            2
        );
        assert_eq!(value["ops_per_sec"], 3.5);
        for key in ["p50", "p90", "p99", "p99.9", "max"] {
            assert!(value["latency_us"][key].is_number(), "{key}");
        }
        assert!(value.get("first_fill_to_block_us").is_none());
    }

    #[test]
    fn breach_fields_are_reported_when_asked() {
        let breach = BreachReport {
            blocked: true,
            first_fill_to_block: Some(Duration::from_millis(2)),
        };
        let value = to_json("breach", &sample(), Duration::from_secs(1), Some(breach));
        assert_eq!(value["first_fill_to_block_us"], 2000.0);
        assert!(
            to_text("breach", &sample(), Duration::from_secs(1), Some(breach)).contains("2000 us")
        );
    }
}
