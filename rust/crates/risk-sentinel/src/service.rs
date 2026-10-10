//! tonic adapter: the `RiskSentinelService.Monitor` bidirectional stream.

use std::pin::Pin;
use std::time::Duration;

use lockstep_proto::v1::monitor_request::Message as Upstream;
use lockstep_proto::v1::monitor_response::Message as Downstream;
use lockstep_proto::v1::risk_sentinel_service_server::RiskSentinelService;
use lockstep_proto::v1::{MonitorRequest, MonitorResponse, SessionAccept, SessionHello};
use lockstep_proto::{PROTOCOL_MAJOR, protocol_version};
use tokio::sync::mpsc;
use tokio::sync::mpsc::error::TrySendError;
use tokio::time::{Instant, MissedTickBehavior, interval_at, sleep};
use tokio_stream::Stream;
use tokio_stream::wrappers::ReceiverStream;
use tonic::{Request, Response, Status, Streaming};
use tracing::{info, warn};

use crate::engine::RiskEngine;
use crate::session::{DEFAULT_DEDUP_CAPACITY, SentinelState, Session, SharedState};

type ResponseStream = Pin<Box<dyn Stream<Item = Result<MonitorResponse, Status>> + Send>>;

/// Downstream buffer per session. Risk commands are rare; if the exchange stops
/// reading, back-pressure reaches the session task, not the whole server.
const DOWNSTREAM_BUFFER: usize = 64;

/// Timers and sizes of a sentinel. The defaults are the protocol's.
#[derive(Debug, Clone, Copy)]
pub struct SessionConfig {
    /// How often a heartbeat is sent downstream.
    pub heartbeat_interval: Duration,
    /// A session with no upstream message for this long is closed.
    pub idle_timeout: Duration,
    /// A command unacknowledged for this long is logged as a warning.
    pub unacknowledged_warn_after: Duration,
    /// Execution reports remembered for duplicate detection.
    pub dedup_capacity: usize,
}

impl Default for SessionConfig {
    fn default() -> Self {
        Self {
            heartbeat_interval: Duration::from_secs(1),
            idle_timeout: Duration::from_secs(5),
            unacknowledged_warn_after: Duration::from_secs(5),
            dedup_capacity: DEFAULT_DEDUP_CAPACITY,
        }
    }
}

/// The Monitor service. One instance serves every session; the risk engine,
/// duplicate window and command ids are shared because positions are global
/// per trader and reports repeat across reconnects.
#[derive(Debug)]
pub struct Sentinel {
    id: String,
    state: SharedState,
    config: SessionConfig,
}

impl Sentinel {
    #[must_use]
    pub fn new(sentinel_id: impl Into<String>, engine: RiskEngine) -> Self {
        Self::with_config(sentinel_id, engine, SessionConfig::default())
    }

    #[must_use]
    pub fn with_config(
        sentinel_id: impl Into<String>,
        engine: RiskEngine,
        config: SessionConfig,
    ) -> Self {
        Self {
            id: sentinel_id.into(),
            state: SentinelState::new(engine, config.dedup_capacity).into_shared(),
            config,
        }
    }
}

/// Validates the first upstream message of a session.
fn expect_hello(first: Option<MonitorRequest>) -> Result<SessionHello, Status> {
    let Some(MonitorRequest {
        message: Some(Upstream::Hello(hello)),
    }) = first
    else {
        return Err(Status::failed_precondition(
            "first message of a Monitor session must be hello",
        ));
    };
    let major = hello.protocol.map_or(0, |p| p.major);
    if major != PROTOCOL_MAJOR {
        return Err(Status::failed_precondition(format!(
            "unsupported protocol major {major}, sentinel speaks {PROTOCOL_MAJOR}"
        )));
    }
    Ok(hello)
}

#[tonic::async_trait]
impl RiskSentinelService for Sentinel {
    type MonitorStream = ResponseStream;

    async fn monitor(
        &self,
        request: Request<Streaming<MonitorRequest>>,
    ) -> Result<Response<Self::MonitorStream>, Status> {
        let mut upstream = request.into_inner();
        let hello = expect_hello(upstream.message().await?)?;
        info!(
            exchange_id = %hello.exchange_id,
            shard_count = hello.shard_count,
            "session established"
        );

        let (tx, rx) = mpsc::channel(DOWNSTREAM_BUFFER);
        let accept = MonitorResponse {
            message: Some(Downstream::Accept(SessionAccept {
                protocol: Some(protocol_version()),
                sentinel_id: self.id.clone(),
            })),
        };
        tx.send(Ok(accept))
            .await
            .map_err(|_| Status::cancelled("client went away during handshake"))?;

        let session = Session::new(std::sync::Arc::clone(&self.state));
        tokio::spawn(run_session(
            upstream,
            tx,
            session,
            hello.exchange_id,
            self.config,
        ));

        Ok(Response::new(Box::pin(ReceiverStream::new(rx))))
    }
}

/// Drives one session until the exchange closes it, a transport error
/// occurs, the idle timeout fires, or the exchange stops reading.
async fn run_session(
    mut upstream: Streaming<MonitorRequest>,
    downstream: mpsc::Sender<Result<MonitorResponse, Status>>,
    mut session: Session,
    exchange_id: String,
    config: SessionConfig,
) {
    let mut heartbeat = interval_at(
        Instant::now() + config.heartbeat_interval,
        config.heartbeat_interval,
    );
    heartbeat.set_missed_tick_behavior(MissedTickBehavior::Skip);
    let idle = sleep(config.idle_timeout);
    tokio::pin!(idle);

    'session: loop {
        tokio::select! {
            message = upstream.message() => match message {
                Ok(Some(message)) => {
                    idle.as_mut().reset(Instant::now() + config.idle_timeout);
                    for response in session.handle(message).await {
                        if downstream.send(Ok(response)).await.is_err() {
                            // The exchange closed its side: nothing left to deliver.
                            break 'session;
                        }
                    }
                }
                Ok(None) => break,
                Err(status) => {
                    warn!(%exchange_id, %status, "session error");
                    break;
                }
            },
            _ = heartbeat.tick() => {
                session
                    .warn_unacknowledged(Instant::now(), config.unacknowledged_warn_after)
                    .await;
                // A full buffer means the exchange is not reading; commands are
                // already queued ahead of this heartbeat, so skip it.
                if let Err(TrySendError::Closed(_)) = downstream.try_send(Ok(Session::heartbeat())) {
                    break;
                }
            }
            () = &mut idle => {
                warn!(%exchange_id, timeout = ?config.idle_timeout, "no upstream message, closing session");
                let _ = downstream.try_send(Err(Status::deadline_exceeded(
                    "no upstream message within the idle timeout",
                )));
                break;
            }
        }
    }
    info!(%exchange_id, "session closed");
}
