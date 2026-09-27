//! loadgen: submits orders to exchange-core's `OrderEntryService` and reports
//! round-trip latency percentiles.
//!
//! SKELETON STATUS: sequential submission of identical orders; this is what the
//! end-to-end smoke test drives. Concurrency, rate control and randomised order
//! flow: docs/tasks/017-loadgen.md.

use std::time::{Duration, Instant};

use anyhow::{Context, bail};
use clap::{Parser, ValueEnum};
use hdrhistogram::Histogram;
use lockstep_proto::v1::command_ack::Result as AckResult;
use lockstep_proto::v1::order_entry_service_client::OrderEntryServiceClient;
use lockstep_proto::v1::{OrderType, Side, SubmitOrderRequest, TimeInForce};
use lockstep_proto::{PROTOCOL_MAJOR, PROTOCOL_METADATA_KEY, PROTOCOL_MINOR};
use tonic::metadata::MetadataValue;

#[derive(Debug, Clone, Copy, ValueEnum)]
enum CliSide {
    Buy,
    Sell,
}

#[derive(Debug, Parser)]
#[command(
    version,
    about = "Order-entry load generator for lockstep exchange-core"
)]
struct Args {
    /// exchange-core order entry endpoint.
    #[arg(long, default_value = "http://127.0.0.1:50051")]
    target: String,
    /// Number of orders to submit.
    #[arg(long, default_value_t = 1)]
    count: u64,
    #[arg(long, default_value_t = 1)]
    trader: u64,
    #[arg(long, default_value_t = 1)]
    instrument: u32,
    #[arg(long, value_enum, default_value = "buy")]
    side: CliSide,
    /// Limit price in ticks.
    #[arg(long, default_value_t = 100)]
    price: i64,
    #[arg(long, default_value_t = 10)]
    quantity: u64,
    /// Exit with an error unless every order is accepted.
    #[arg(long)]
    expect_accepted: bool,
    /// Give up connecting after this long (the exchange may still be starting).
    #[arg(long, default_value = "10")]
    connect_timeout_secs: u64,
}

async fn connect(
    target: &str,
    timeout: Duration,
) -> anyhow::Result<OrderEntryServiceClient<tonic::transport::Channel>> {
    let deadline = Instant::now() + timeout;
    loop {
        match OrderEntryServiceClient::connect(target.to_owned()).await {
            Ok(client) => return Ok(client),
            Err(err) if Instant::now() < deadline => {
                eprintln!("waiting for {target}: {err}");
                tokio::time::sleep(Duration::from_millis(200)).await;
            }
            Err(err) => return Err(err).context(format!("cannot connect to {target}")),
        }
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    let mut client = connect(&args.target, Duration::from_secs(args.connect_timeout_secs)).await?;
    let version: MetadataValue<_> = format!("{PROTOCOL_MAJOR}.{PROTOCOL_MINOR}").parse()?;

    let mut latencies = Histogram::<u64>::new(3)?;
    let (mut accepted, mut rejected) = (0_u64, 0_u64);

    for client_order_id in 1..=args.count {
        let mut request = tonic::Request::new(SubmitOrderRequest {
            trader_id: args.trader,
            client_order_id,
            instrument_id: args.instrument,
            side: match args.side {
                CliSide::Buy => Side::Buy,
                CliSide::Sell => Side::Sell,
            }
            .into(),
            r#type: OrderType::Limit.into(),
            time_in_force: TimeInForce::Gtc.into(),
            price_ticks: args.price,
            quantity: args.quantity,
        });
        request
            .metadata_mut()
            .insert(PROTOCOL_METADATA_KEY, version.clone());

        let started = Instant::now();
        let response = client.submit_order(request).await?.into_inner();
        latencies.record(u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX))?;

        match response.ack.and_then(|ack| ack.result) {
            Some(AckResult::Accepted(ok)) => {
                accepted += 1;
                if args.count <= 10 {
                    println!(
                        "order {client_order_id}: accepted, order_id={}",
                        ok.order_id
                    );
                }
            }
            Some(AckResult::Rejected(rejection)) => {
                rejected += 1;
                println!(
                    "order {client_order_id}: rejected: {} ({})",
                    rejection.detail, rejection.reason
                );
            }
            None => bail!("order {client_order_id}: empty ack"),
        }
    }

    println!(
        "submitted={} accepted={accepted} rejected={rejected} p50={}us p99={}us p99.9={}us",
        args.count,
        latencies.value_at_quantile(0.50) / 1_000,
        latencies.value_at_quantile(0.99) / 1_000,
        latencies.value_at_quantile(0.999) / 1_000,
    );
    if args.expect_accepted && rejected > 0 {
        bail!("{rejected} order(s) rejected but --expect-accepted was given");
    }
    Ok(())
}
