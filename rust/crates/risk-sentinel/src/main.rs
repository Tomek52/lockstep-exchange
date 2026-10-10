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
    /// Absolute net position (lots) per trader and instrument before the
    /// trader is blocked.
    #[arg(long, default_value_t = RiskLimits::default().max_abs_position, allow_negative_numbers = true)]
    max_abs_position: i128,
    /// Loss (tick-lots) per trader before the trader is blocked.
    #[arg(long, default_value_t = RiskLimits::default().max_trader_loss, allow_negative_numbers = true)]
    max_trader_loss: i128,
    /// Aggregate loss (tick-lots) across all traders that engages the kill
    /// switch.
    #[arg(long, default_value_t = RiskLimits::default().kill_switch_loss, allow_negative_numbers = true)]
    kill_switch_loss: i128,
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

    let limits = RiskLimits {
        max_abs_position: args.max_abs_position,
        max_trader_loss: args.max_trader_loss,
        kill_switch_loss: args.kill_switch_loss,
    };
    limits.validate().map_err(anyhow::Error::msg)?;
    info!(?limits, "risk limits");

    let sentinel = Sentinel::new(args.sentinel_id, RiskEngine::new(limits));
    info!(listen = %args.listen, "risk-sentinel listening");
    tonic::transport::Server::builder()
        .add_service(RiskSentinelServiceServer::new(sentinel))
        .serve_with_shutdown(args.listen, shutdown_signal())
        .await
        .context("gRPC server failed")?;
    info!("risk-sentinel stopped");
    Ok(())
}
