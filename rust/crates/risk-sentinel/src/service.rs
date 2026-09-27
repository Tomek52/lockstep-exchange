//! tonic adapter: the `RiskSentinelService.Monitor` bidirectional stream.

use std::pin::Pin;
use std::sync::Arc;

use lockstep_proto::v1::monitor_request::Message as Upstream;
use lockstep_proto::v1::monitor_response::Message as Downstream;
use lockstep_proto::v1::risk_sentinel_service_server::RiskSentinelService;
use lockstep_proto::v1::{MonitorRequest, MonitorResponse, SessionAccept, SessionHello};
use lockstep_proto::{PROTOCOL_MAJOR, protocol_version};
use tokio::sync::{Mutex, mpsc};
use tokio_stream::Stream;
use tokio_stream::wrappers::ReceiverStream;
use tonic::{Request, Response, Status, Streaming};
use tracing::{info, warn};

use crate::engine::RiskEngine;

type ResponseStream = Pin<Box<dyn Stream<Item = Result<MonitorResponse, Status>> + Send>>;

/// Downstream buffer per session. Risk commands are rare; if the exchange stops
/// reading, back-pressure reaches the session task, not the whole server.
const DOWNSTREAM_BUFFER: usize = 64;

/// The Monitor service. One instance serves every session; the risk engine is
/// shared because positions are global per trader, whichever exchange instance
/// reported the fill.
#[derive(Debug)]
pub struct Sentinel {
    sentinel_id: String,
    engine: Arc<Mutex<RiskEngine>>,
}

impl Sentinel {
    #[must_use]
    pub fn new(sentinel_id: impl Into<String>, engine: RiskEngine) -> Self {
        Self {
            sentinel_id: sentinel_id.into(),
            engine: Arc::new(Mutex::new(engine)),
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
                sentinel_id: self.sentinel_id.clone(),
            })),
        };
        tx.send(Ok(accept))
            .await
            .map_err(|_| Status::cancelled("client went away during handshake"))?;

        let engine = Arc::clone(&self.engine);
        let exchange_id = hello.exchange_id;
        tokio::spawn(async move {
            loop {
                match upstream.message().await {
                    Ok(Some(message)) => handle_upstream(&message, &engine, &tx),
                    Ok(None) => break,
                    Err(status) => {
                        warn!(%exchange_id, %status, "session error");
                        break;
                    }
                }
            }
            info!(%exchange_id, "session closed");
        });

        Ok(Response::new(Box::pin(ReceiverStream::new(rx))))
    }
}

fn handle_upstream(
    message: &MonitorRequest,
    _engine: &Arc<Mutex<RiskEngine>>,
    _downstream: &mpsc::Sender<Result<MonitorResponse, Status>>,
) {
    match &message.message {
        Some(Upstream::Execution(_report)) => {
            // TODO(task-016): decode into engine::Fill, run RiskEngine::on_fill,
            // send resulting RiskCommands downstream with fresh command ids.
        }
        Some(Upstream::CommandApplied(applied)) => {
            info!(
                command_id = applied.command_id,
                "risk command applied by exchange"
            );
        }
        Some(Upstream::Heartbeat(_)) | None => {}
        Some(Upstream::Hello(_)) => warn!("duplicate hello ignored"),
    }
}
