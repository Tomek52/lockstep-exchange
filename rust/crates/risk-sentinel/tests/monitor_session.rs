//! In-process gRPC tests of a whole Monitor session over a real loopback
//! socket: reports in, risk commands and heartbeats out.

use std::time::Duration;

use lockstep_proto::protocol_version;
use lockstep_proto::v1::monitor_request::Message as Upstream;
use lockstep_proto::v1::monitor_response::Message as Downstream;
use lockstep_proto::v1::risk_command::Action;
use lockstep_proto::v1::risk_sentinel_service_client::RiskSentinelServiceClient;
use lockstep_proto::v1::risk_sentinel_service_server::RiskSentinelServiceServer;
use lockstep_proto::v1::{
    BlockTrader, CommandApplied, ExecutionReport, Heartbeat, MonitorRequest, MonitorResponse,
    SessionHello, Side,
};
use risk_sentinel::engine::RiskEngine;
use risk_sentinel::limits::RiskLimits;
use risk_sentinel::service::{Sentinel, SessionConfig};
use tokio::net::TcpListener;
use tokio::sync::mpsc;
use tokio::time::timeout;
use tokio_stream::wrappers::{ReceiverStream, TcpListenerStream};
use tonic::Streaming;

const WAIT: Duration = Duration::from_secs(10);

fn limits() -> RiskLimits {
    RiskLimits {
        max_abs_position: 10,
        max_trader_loss: 1_000_000,
        kill_switch_loss: 10_000_000,
    }
}

async fn start_server(config: SessionConfig) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let sentinel = Sentinel::with_config("test-sentinel", RiskEngine::new(limits()), config);
    tokio::spawn(
        tonic::transport::Server::builder()
            .add_service(RiskSentinelServiceServer::new(sentinel))
            .serve_with_incoming(TcpListenerStream::new(listener)),
    );
    format!("http://{addr}")
}

/// A connected session: send upstream messages, read downstream ones.
struct Client {
    upstream: mpsc::Sender<MonitorRequest>,
    downstream: Streaming<MonitorResponse>,
}

impl Client {
    async fn connect(url: &str) -> Self {
        let mut grpc = RiskSentinelServiceClient::connect(url.to_owned())
            .await
            .unwrap();
        let (upstream, rx) = mpsc::channel(16);
        upstream
            .send(MonitorRequest {
                message: Some(Upstream::Hello(SessionHello {
                    protocol: Some(protocol_version()),
                    exchange_id: "exchange-under-test".into(),
                    shard_count: 2,
                })),
            })
            .await
            .unwrap();
        let downstream = grpc
            .monitor(ReceiverStream::new(rx))
            .await
            .unwrap()
            .into_inner();
        Self {
            upstream,
            downstream,
        }
    }

    async fn send(&self, message: Upstream) {
        self.upstream
            .send(MonitorRequest {
                message: Some(message),
            })
            .await
            .unwrap();
    }

    /// The next downstream message, failing the test if none arrives.
    async fn next(&mut self) -> Downstream {
        timeout(WAIT, self.downstream.message())
            .await
            .expect("timed out waiting for a downstream message")
            .unwrap()
            .expect("stream ended")
            .message
            .expect("empty downstream message")
    }

    /// The next message that is not a heartbeat.
    async fn next_non_heartbeat(&mut self) -> Downstream {
        loop {
            match self.next().await {
                Downstream::Heartbeat(_) => {}
                other => return other,
            }
        }
    }
}

fn report(trader: u64, sequence: u64, quantity: u64) -> Upstream {
    Upstream::Execution(ExecutionReport {
        shard_id: 0,
        shard_sequence: sequence,
        timestamp_ns: 1,
        trader_id: trader,
        instrument_id: 1,
        order_id: 1,
        side: Side::Buy as i32,
        price_ticks: 100,
        quantity,
        is_maker: false,
    })
}

fn expect_block(message: Downstream, command_id: u64, trader_id: u64) {
    let Downstream::Command(command) = message else {
        panic!("expected a risk command, got {message:?}");
    };
    assert_eq!(command.command_id, command_id);
    assert_eq!(
        command.action,
        Some(Action::BlockTrader(BlockTrader { trader_id }))
    );
}

#[tokio::test]
async fn breaching_report_is_answered_with_a_block_command() {
    let mut client = Client::connect(&start_server(SessionConfig::default()).await).await;

    assert!(matches!(client.next().await, Downstream::Accept(_)));
    client.send(report(42, 1, 11)).await;

    expect_block(client.next_non_heartbeat().await, 1, 42);
}

#[tokio::test]
async fn acknowledgement_does_not_produce_a_reply() {
    let mut client = Client::connect(&start_server(SessionConfig::default()).await).await;
    assert!(matches!(client.next().await, Downstream::Accept(_)));
    client.send(report(42, 1, 11)).await;
    expect_block(client.next_non_heartbeat().await, 1, 42);

    client
        .send(Upstream::CommandApplied(CommandApplied { command_id: 1 }))
        .await;
    // The next breach still gets the next id: the session is alive and sane.
    client.send(report(43, 2, 11)).await;

    expect_block(client.next_non_heartbeat().await, 2, 43);
}

#[tokio::test]
async fn reports_replayed_on_a_new_session_are_ignored_and_ids_continue() {
    let url = start_server(SessionConfig::default()).await;
    let mut first = Client::connect(&url).await;
    assert!(matches!(first.next().await, Downstream::Accept(_)));
    first.send(report(42, 1, 6)).await;
    // Unacknowledged and then lost: the exchange reconnects and replays.
    drop(first);

    let mut second = Client::connect(&url).await;
    assert!(matches!(second.next().await, Downstream::Accept(_)));
    second.send(report(42, 1, 6)).await; // replay: 6 lots must count once
    second.send(report(42, 2, 5)).await; // 6 + 5 = 11 > 10

    expect_block(second.next_non_heartbeat().await, 1, 42);
}

#[tokio::test]
async fn heartbeats_are_sent_downstream() {
    let config = SessionConfig {
        heartbeat_interval: Duration::from_millis(30),
        ..SessionConfig::default()
    };
    let mut client = Client::connect(&start_server(config).await).await;
    assert!(matches!(client.next().await, Downstream::Accept(_)));

    for _ in 0..3 {
        assert!(matches!(client.next().await, Downstream::Heartbeat(_)));
    }
}

#[tokio::test]
async fn silent_session_is_closed_after_the_idle_timeout() {
    let config = SessionConfig {
        heartbeat_interval: Duration::from_millis(20),
        idle_timeout: Duration::from_millis(200),
        ..SessionConfig::default()
    };
    let mut client = Client::connect(&start_server(config).await).await;
    assert!(matches!(client.next().await, Downstream::Accept(_)));

    let status = loop {
        match timeout(WAIT, client.downstream.message())
            .await
            .expect("session was not closed")
        {
            Ok(Some(_heartbeat)) => {}
            Ok(None) => panic!("closed without a status"),
            Err(status) => break status,
        }
    };
    assert_eq!(status.code(), tonic::Code::DeadlineExceeded);
}

#[tokio::test]
async fn upstream_heartbeats_keep_the_session_open() {
    let config = SessionConfig {
        heartbeat_interval: Duration::from_millis(20),
        idle_timeout: Duration::from_millis(200),
        ..SessionConfig::default()
    };
    let mut client = Client::connect(&start_server(config).await).await;
    assert!(matches!(client.next().await, Downstream::Accept(_)));

    // 600 ms of traffic, three times the idle timeout.
    for _ in 0..12 {
        client
            .send(Upstream::Heartbeat(Heartbeat { sent_at_ns: 1 }))
            .await;
        tokio::time::sleep(Duration::from_millis(50)).await;
    }

    client.send(report(42, 1, 11)).await;
    expect_block(client.next_non_heartbeat().await, 1, 42);
}
