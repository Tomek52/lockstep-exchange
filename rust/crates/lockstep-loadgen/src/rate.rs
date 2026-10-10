//! Rate control shared by all workers.
//!
//! A [`Pacer`] hands out evenly spaced time slots. Slots are claimed from one
//! shared counter, so the aggregate rate is the configured rate no matter how
//! many workers there are. A worker that runs late does not skip slots: the
//! pacer lets it catch up, so the long-run rate stays exact.

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

#[derive(Debug)]
pub(crate) struct Pacer {
    start: Instant,
    /// Nanoseconds between two consecutive slots; `None` means unlimited.
    interval_ns: Option<u64>,
    next_slot: AtomicU64,
}

impl Pacer {
    /// `ops_per_sec` of `None` (or a non-positive rate) never delays anybody.
    pub(crate) fn new(start: Instant, ops_per_sec: Option<f64>) -> Self {
        let interval_ns = ops_per_sec.filter(|rate| *rate > 0.0).map(|rate| {
            // Sub-nanosecond intervals (> 1e9 ops/s) are effectively unlimited.
            #[expect(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
            let ns = (1e9 / rate).max(1.0) as u64;
            ns
        });
        Self {
            start,
            interval_ns,
            next_slot: AtomicU64::new(0),
        }
    }

    /// Claims the next slot and returns the instant it opens.
    pub(crate) fn claim(&self) -> Instant {
        match self.interval_ns {
            None => self.start,
            Some(interval) => {
                // Relaxed: the counter only hands out distinct numbers; it
                // guards no other data.
                let n = self.next_slot.fetch_add(1, Ordering::Relaxed);
                self.start + Duration::from_nanos(n.saturating_mul(interval))
            }
        }
    }

    /// Waits for the next slot. Returns immediately when unlimited or late.
    pub(crate) async fn acquire(&self) {
        if self.interval_ns.is_some() {
            tokio::time::sleep_until(self.claim().into()).await;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn within_five_percent(actual: u64, expected: u64) -> bool {
        actual.abs_diff(expected) * 20 <= expected
    }

    /// Wall-clock tolerance: a loaded CI runner may deliver timers late.
    fn within_fifteen_percent(actual: u64, expected: u64) -> bool {
        actual.abs_diff(expected) * 100 <= expected * 15
    }

    #[test]
    fn slots_are_evenly_spaced_and_hit_the_target_count_over_two_seconds() {
        let start = Instant::now();
        let pacer = Pacer::new(start, Some(500.0));
        let window = start + Duration::from_secs(2);
        let mut issued = 0_u64;
        while pacer.claim() < window {
            issued += 1;
        }
        assert!(within_five_percent(issued, 1_000), "issued {issued}");
    }

    #[test]
    fn unlimited_pacer_never_delays() {
        let start = Instant::now();
        let pacer = Pacer::new(start, None);
        assert_eq!(pacer.claim(), start);
        assert_eq!(pacer.claim(), start);
    }

    #[tokio::test]
    async fn real_time_rate_over_two_seconds_with_concurrent_workers_is_close() {
        let started = Instant::now();
        let pacer = std::sync::Arc::new(Pacer::new(started, Some(400.0)));
        let deadline = started + Duration::from_secs(2);
        let mut workers = Vec::new();
        for _ in 0..4 {
            let pacer = pacer.clone();
            workers.push(tokio::spawn(async move {
                let mut done = 0_u64;
                while Instant::now() < deadline {
                    pacer.acquire().await;
                    if Instant::now() < deadline {
                        done += 1;
                    }
                }
                done
            }));
        }
        let mut total = 0;
        for worker in workers {
            total += worker.await.unwrap();
        }
        assert!(within_fifteen_percent(total, 800), "issued {total}");
    }
}
