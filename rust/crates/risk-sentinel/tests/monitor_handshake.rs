//! In-process gRPC test of the Monitor handshake over a real loopback socket.

use lockstep_proto::protocol_version;
use lockstep_proto::v1::monitor_request::Message as Upstream;
use lockstep_proto::v1::monitor_response::Message as Downstream;
use lockstep_proto::v1::risk_sentinel_service_client::RiskSentinelServiceClient;
use lockstep_proto::v1::risk_sentinel_service_server::RiskSentinelServiceServer;
use lockstep_proto::v1::{MonitorRequest, ProtocolVersion, SessionHello};
use risk_sentinel::engine::RiskEngine;
use risk_sentinel::service::Sentinel;
use tokio::net::TcpListener;
use tokio_stream::wrappers::TcpListenerStream;

async fn start_server() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let service =
        RiskSentinelServiceServer::new(Sentinel::new("test-sentinel", RiskEngine::default()));
    tokio::spawn(
        tonic::transport::Server::builder()
            .add_service(service)
            .serve_with_incoming(TcpListenerStream::new(listener)),
    );
    format!("http://{addr}")
}

fn hello(protocol: ProtocolVersion) -> MonitorRequest {
    MonitorRequest {
        message: Some(Upstream::Hello(SessionHello {
            protocol: Some(protocol),
            exchange_id: "exchange-under-test".into(),
            shard_count: 2,
        })),
    }
}

#[tokio::test]
async fn hello_is_answered_with_accept() {
    let mut client = RiskSentinelServiceClient::connect(start_server().await)
        .await
        .unwrap();

    let upstream = tokio_stream::iter(vec![hello(protocol_version())]);
    let mut downstream = client.monitor(upstream).await.unwrap().into_inner();

    let first = downstream.message().await.unwrap().expect("accept message");
    let Some(Downstream::Accept(accept)) = first.message else {
        panic!("expected accept, got {first:?}");
    };
    assert_eq!(accept.sentinel_id, "test-sentinel");
    assert_eq!(accept.protocol, Some(protocol_version()));
}

#[tokio::test]
async fn incompatible_major_version_is_refused() {
    let mut client = RiskSentinelServiceClient::connect(start_server().await)
        .await
        .unwrap();
    let upstream = tokio_stream::iter(vec![hello(ProtocolVersion { major: 2, minor: 0 })]);

    let status = client.monitor(upstream).await.expect_err("must be refused");
    assert_eq!(status.code(), tonic::Code::FailedPrecondition);
}
