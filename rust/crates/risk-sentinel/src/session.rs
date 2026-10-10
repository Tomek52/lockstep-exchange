//! Monitor session logic: decodes upstream messages, runs the risk engine and
//! produces the downstream messages. No sockets here, so it is unit tested
//! without a network (`service` owns the stream and the timers).
//!
//! State that must outlive one connection lives in [`SentinelState`], shared
//! by every [`Session`]: the risk engine (positions are keyed by trader), the
//! duplicate window (reports are replayed after a reconnect), the command id
//! counter (unique per sentinel process) and the pending commands.

use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use lockstep_proto::v1::monitor_request::Message as Upstream;
use lockstep_proto::v1::monitor_response::Message as Downstream;
use lockstep_proto::v1::risk_command::Action;
use lockstep_proto::v1::{
    BlockTrader, ExecutionReport, Heartbeat, KillSwitch, MonitorRequest, MonitorResponse,
    RiskCommand,
};
use tokio::sync::Mutex;
use tokio::time::Instant;
use tracing::{debug, info, warn};

use crate::engine::{Fill, RiskAction, RiskEngine};
use crate::positions::Side;

/// Reports remembered for duplicate detection (FIFO eviction).
pub const DEFAULT_DEDUP_CAPACITY: usize = 1_000_000;

/// `(shard_id, shard_sequence, is_maker)`: one trade yields a maker and a
/// taker report with the same shard id and sequence (ADR-0013).
type ReportKey = (u32, u64, bool);

/// A bounded set of recently seen keys. Once full, the oldest key is
/// forgotten, so a duplicate that arrives after `capacity` newer reports is
/// processed again; the window only has to cover a reconnect replay.
#[derive(Debug)]
struct DedupWindow {
    capacity: usize,
    seen: HashSet<ReportKey>,
    order: VecDeque<ReportKey>,
}

impl DedupWindow {
    fn new(capacity: usize) -> Self {
        let capacity = capacity.max(1);
        Self {
            capacity,
            seen: HashSet::new(),
            order: VecDeque::new(),
        }
    }

    /// Records `key`; returns false if it is already in the window.
    fn first_seen(&mut self, key: ReportKey) -> bool {
        if !self.seen.insert(key) {
            return false;
        }
        self.order.push_back(key);
        if self.order.len() > self.capacity
            && let Some(oldest) = self.order.pop_front()
        {
            self.seen.remove(&oldest);
        }
        true
    }
}

/// A command sent downstream and not yet acknowledged.
#[derive(Debug)]
struct Pending {
    action: RiskAction,
    sent_at: Instant,
    /// The unacknowledged warning is logged once per command.
    warned: bool,
}

/// Everything shared between sessions of one sentinel process.
#[derive(Debug)]
pub struct SentinelState {
    engine: RiskEngine,
    dedup: DedupWindow,
    next_command_id: u64,
    pending: HashMap<u64, Pending>,
}

/// Handle to the shared state.
pub type SharedState = Arc<Mutex<SentinelState>>;

impl SentinelState {
    #[must_use]
    pub fn new(engine: RiskEngine, dedup_capacity: usize) -> Self {
        Self {
            engine,
            dedup: DedupWindow::new(dedup_capacity),
            next_command_id: 1,
            pending: HashMap::new(),
        }
    }

    #[must_use]
    pub fn into_shared(self) -> SharedState {
        Arc::new(Mutex::new(self))
    }

    fn next_id(&mut self) -> u64 {
        let id = self.next_command_id;
        self.next_command_id += 1;
        id
    }
}

/// One Monitor session (one connection of one exchange instance).
#[derive(Debug)]
pub struct Session {
    state: SharedState,
}

impl Session {
    #[must_use]
    pub fn new(state: SharedState) -> Self {
        Self { state }
    }

    /// Handles one upstream message and returns the downstream messages to
    /// send for it, in order.
    pub async fn handle(&mut self, message: MonitorRequest) -> Vec<MonitorResponse> {
        match message.message {
            Some(Upstream::Execution(report)) => self.on_execution(&report).await,
            Some(Upstream::CommandApplied(applied)) => {
                self.on_command_applied(applied.command_id).await;
                Vec::new()
            }
            Some(Upstream::Heartbeat(_)) | None => Vec::new(),
            Some(Upstream::Hello(_)) => {
                warn!("duplicate hello ignored");
                Vec::new()
            }
        }
    }

    /// The heartbeat to send downstream.
    #[must_use]
    pub fn heartbeat() -> MonitorResponse {
        let sent_at_ns = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .ok()
            .and_then(|elapsed| i64::try_from(elapsed.as_nanos()).ok())
            .unwrap_or(0);
        MonitorResponse {
            message: Some(Downstream::Heartbeat(Heartbeat { sent_at_ns })),
        }
    }

    /// Logs a warning, once per command, for every command that has been
    /// pending for longer than `warn_after` at `now`. Commands are not
    /// resent: the exchange journals and applies what it received. Returns
    /// the number of commands warned about by this call.
    pub async fn warn_unacknowledged(&self, now: Instant, warn_after: Duration) -> usize {
        let mut state = self.state.lock().await;
        let mut warned = 0;
        for (command_id, pending) in &mut state.pending {
            let age = now.saturating_duration_since(pending.sent_at);
            if !pending.warned && age >= warn_after {
                pending.warned = true;
                warned += 1;
                warn!(
                    command_id,
                    age_ms = u64::try_from(age.as_millis()).unwrap_or(u64::MAX),
                    action = ?pending.action,
                    "risk command not acknowledged"
                );
            }
        }
        warned
    }

    /// Commands sent and not yet acknowledged, across all sessions.
    pub async fn pending_commands(&self) -> usize {
        self.state.lock().await.pending.len()
    }

    async fn on_execution(&mut self, report: &ExecutionReport) -> Vec<MonitorResponse> {
        let Some(fill) = decode_fill(report) else {
            return Vec::new();
        };
        let key = (report.shard_id, report.shard_sequence, report.is_maker);

        let mut state = self.state.lock().await;
        if !state.dedup.first_seen(key) {
            debug!(
                shard_id = key.0,
                shard_sequence = key.1,
                is_maker = key.2,
                "duplicate execution report ignored"
            );
            return Vec::new();
        }

        let now = Instant::now();
        let mut out = Vec::new();
        for action in state.engine.on_fill(&fill) {
            let command_id = state.next_id();
            out.push(encode_command(command_id, &action));
            state.pending.insert(
                command_id,
                Pending {
                    action,
                    sent_at: now,
                    warned: false,
                },
            );
        }
        out
    }

    async fn on_command_applied(&mut self, command_id: u64) {
        let removed = self.state.lock().await.pending.remove(&command_id);
        if let Some(pending) = removed {
            info!(
                command_id,
                latency_us =
                    u64::try_from(pending.sent_at.elapsed().as_micros()).unwrap_or(u64::MAX),
                "risk command applied by exchange"
            );
        } else {
            warn!(command_id, "acknowledgement for an unknown command");
        }
    }
}

/// Decodes a report into a fill. An unknown side is logged and skipped:
/// it is data from outside, never a reason to panic.
fn decode_fill(report: &ExecutionReport) -> Option<Fill> {
    use lockstep_proto::v1::Side as WireSide;

    let side = match WireSide::try_from(report.side) {
        Ok(WireSide::Buy) => Side::Buy,
        Ok(WireSide::Sell) => Side::Sell,
        Ok(WireSide::Unspecified) | Err(_) => {
            warn!(
                shard_id = report.shard_id,
                shard_sequence = report.shard_sequence,
                side = report.side,
                "execution report with an unknown side skipped"
            );
            return None;
        }
    };
    Some(Fill {
        trader: report.trader_id,
        instrument: report.instrument_id,
        side,
        price: report.price_ticks,
        quantity: report.quantity,
    })
}

fn encode_command(command_id: u64, action: &RiskAction) -> MonitorResponse {
    let (reason, action) = match action {
        RiskAction::BlockTrader { trader, reason } => (
            reason.clone(),
            Action::BlockTrader(BlockTrader { trader_id: *trader }),
        ),
        RiskAction::KillSwitch { engaged, reason } => (
            reason.clone(),
            Action::KillSwitch(KillSwitch { engaged: *engaged }),
        ),
    };
    MonitorResponse {
        message: Some(Downstream::Command(RiskCommand {
            command_id,
            reason,
            action: Some(action),
        })),
    }
}

#[cfg(test)]
mod tests {
    use lockstep_proto::v1::{CommandApplied, Side as WireSide};

    use super::*;
    use crate::limits::RiskLimits;

    fn session_with(limits: RiskLimits) -> Session {
        Session::new(SentinelState::new(RiskEngine::new(limits), 1000).into_shared())
    }

    fn limits() -> RiskLimits {
        RiskLimits {
            max_abs_position: 10,
            max_trader_loss: 1_000_000,
            kill_switch_loss: 10_000_000,
        }
    }

    fn report(trader: u64, sequence: u64, quantity: u64) -> ExecutionReport {
        ExecutionReport {
            shard_id: 0,
            shard_sequence: sequence,
            timestamp_ns: 0,
            trader_id: trader,
            instrument_id: 1,
            order_id: 1,
            side: WireSide::Buy as i32,
            price_ticks: 100,
            quantity,
            is_maker: false,
        }
    }

    fn execution(report: ExecutionReport) -> MonitorRequest {
        MonitorRequest {
            message: Some(Upstream::Execution(report)),
        }
    }

    fn command_of(response: &MonitorResponse) -> &RiskCommand {
        let Some(Downstream::Command(command)) = &response.message else {
            panic!("expected a command, got {response:?}");
        };
        command
    }

    #[tokio::test]
    async fn breach_yields_one_block_command_with_id_one() {
        let mut session = session_with(limits());

        let out = session.handle(execution(report(7, 1, 11))).await;

        assert_eq!(out.len(), 1);
        let command = command_of(&out[0]);
        assert_eq!(command.command_id, 1);
        assert_eq!(
            command.action,
            Some(Action::BlockTrader(BlockTrader { trader_id: 7 }))
        );
        assert!(!command.reason.is_empty());
        assert_eq!(session.pending_commands().await, 1);
    }

    #[tokio::test]
    async fn next_breach_from_another_trader_gets_the_next_id() {
        let mut session = session_with(limits());

        let first = session.handle(execution(report(7, 1, 11))).await;
        let second = session.handle(execution(report(8, 2, 11))).await;

        assert_eq!(command_of(&first[0]).command_id, 1);
        assert_eq!(command_of(&second[0]).command_id, 2);
        assert_eq!(
            command_of(&second[0]).action,
            Some(Action::BlockTrader(BlockTrader { trader_id: 8 }))
        );
    }

    #[tokio::test]
    async fn duplicate_report_is_not_applied_twice() {
        // 6 lots twice would be 12 > 10 and block; a duplicate counts once.
        let mut session = session_with(limits());

        let first = session.handle(execution(report(7, 1, 6))).await;
        let duplicate = session.handle(execution(report(7, 1, 6))).await;
        let next = session.handle(execution(report(7, 2, 4))).await;

        assert!(first.is_empty());
        assert!(duplicate.is_empty());
        assert!(next.is_empty(), "6 + 4 = 10 is within the limit");
    }

    #[tokio::test]
    async fn maker_and_taker_reports_of_one_trade_are_both_applied() {
        let mut session = session_with(limits());
        let taker = report(7, 1, 6);
        let maker = ExecutionReport {
            is_maker: true,
            trader_id: 8,
            ..taker
        };

        assert!(session.handle(execution(taker)).await.is_empty());
        assert!(session.handle(execution(maker)).await.is_empty());
        // Trader 8 holds 6 from the maker report: a further 5 breaches.
        let out = session.handle(execution(report(8, 2, 5))).await;
        assert_eq!(out.len(), 1);
    }

    #[tokio::test]
    async fn command_applied_clears_the_pending_entry() {
        let mut session = session_with(limits());
        let out = session.handle(execution(report(7, 1, 11))).await;
        let command_id = command_of(&out[0]).command_id;
        assert_eq!(session.pending_commands().await, 1);

        let ack = MonitorRequest {
            message: Some(Upstream::CommandApplied(CommandApplied { command_id })),
        };
        assert!(session.handle(ack).await.is_empty());

        assert_eq!(session.pending_commands().await, 0);
    }

    #[tokio::test]
    async fn acknowledgement_of_an_unknown_command_is_harmless() {
        let mut session = session_with(limits());
        let ack = MonitorRequest {
            message: Some(Upstream::CommandApplied(CommandApplied { command_id: 99 })),
        };

        assert!(session.handle(ack).await.is_empty());
        assert_eq!(session.pending_commands().await, 0);
    }

    #[tokio::test]
    async fn unknown_side_is_skipped_without_panicking() {
        let mut session = session_with(limits());
        for side in [WireSide::Unspecified as i32, 42] {
            let bad = ExecutionReport {
                side,
                ..report(7, 1, 1_000)
            };
            assert!(session.handle(execution(bad)).await.is_empty());
        }
        // Nothing was applied: 10 lots is still within the limit.
        assert!(session.handle(execution(report(7, 2, 10))).await.is_empty());
    }

    #[tokio::test]
    async fn dedup_window_forgets_the_oldest_key_when_full() {
        let mut window = DedupWindow::new(2);

        assert!(window.first_seen((0, 1, false)));
        assert!(window.first_seen((0, 2, false)));
        assert!(!window.first_seen((0, 1, false)));
        assert!(window.first_seen((0, 3, false)), "evicts key 1");
        assert!(window.first_seen((0, 1, false)), "key 1 was forgotten");
        assert!(!window.first_seen((0, 3, false)));
    }

    #[tokio::test]
    async fn unacknowledged_commands_are_warned_about_once() {
        let mut session = session_with(limits());
        session.handle(execution(report(7, 1, 11))).await;
        let sent = Instant::now();
        let limit = Duration::from_secs(5);

        assert_eq!(session.warn_unacknowledged(sent, limit).await, 0);
        let later = sent + Duration::from_secs(6);
        assert_eq!(session.warn_unacknowledged(later, limit).await, 1);
        assert_eq!(session.warn_unacknowledged(later, limit).await, 0);
        assert_eq!(
            session.pending_commands().await,
            1,
            "never dropped or resent"
        );
    }

    #[tokio::test]
    async fn command_ids_and_dedup_are_shared_by_sessions_of_one_sentinel() {
        let state = SentinelState::new(RiskEngine::new(limits()), 1000).into_shared();
        let mut before_reconnect = Session::new(Arc::clone(&state));
        let mut after_reconnect = Session::new(state);

        let first = before_reconnect.handle(execution(report(7, 1, 11))).await;
        let replay = after_reconnect.handle(execution(report(7, 1, 11))).await;
        let other = after_reconnect.handle(execution(report(8, 2, 11))).await;

        assert_eq!(command_of(&first[0]).command_id, 1);
        assert!(replay.is_empty());
        assert_eq!(command_of(&other[0]).command_id, 2);
    }
}
