//! risk-sentinel binary: serves `RiskSentinelService` until SIGINT/SIGTERM.

use std::net::SocketAddr;

use anyhow::Context;
use clap::Parser;
use lockstep_proto::v1::risk_sentinel_service_server::RiskSentinelServiceServer;
use risk_sentinel::engine::RiskEngine;
use risk_sentinel::limits::RiskLimits;
use risk_sentinel::service::Sentinel;
use tokio::signal::unix::{SignalKind, signal};
use tracing::info;

#[derive(Debug, Parser)]
#[command(version, about = "Post-trade risk sentinel for lockstep exchange-core")]
struct Args {
    /// Address to serve `RiskSentinelService` on.
    #[arg(long, default_value = "0.0.0.0:50052")]
    listen: SocketAddr,
    /// Identity reported in the session handshake.
    #[arg(long, default_value = "risk-sentinel-1")]
    sentinel_id: String,
}

async fn shutdown_signal() {
    let mut terminate = signal(SignalKind::terminate()).expect("install SIGTERM handler");
    tokio::select! {
        _ = tokio::signal::ctrl_c() => {}
        _ = terminate.recv() => {}
    }
    info!("shutdown requested");
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .init();
    let args = Args::parse();

    let sentinel = Sentinel::new(args.sentinel_id, RiskEngine::new(RiskLimits::default()));
    info!(listen = %args.listen, "risk-sentinel listening");
    tonic::transport::Server::builder()
        .add_service(RiskSentinelServiceServer::new(sentinel))
        .serve_with_shutdown(args.listen, shutdown_signal())
        .await
        .context("gRPC server failed")?;
    info!("risk-sentinel stopped");
    Ok(())
}
