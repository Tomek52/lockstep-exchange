//! loadgen: drives order flow into exchange-core's `OrderEntryService` and
//! reports outcome counts and round-trip latency percentiles.
//!
//! Scenarios (`--scenario`): `random` (default), `breach`, `single`.
//! See docs/tasks/017-loadgen.md.

mod rate;
mod report;
mod scenario;

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Context, bail};
use clap::{Parser, ValueEnum};
use lockstep_proto::v1::command_ack::Result as AckResult;
use lockstep_proto::v1::order_entry_service_client::OrderEntryServiceClient;
use lockstep_proto::v1::{
    CommandAck, OrderType, RejectReason, Side, SubmitOrderRequest, TimeInForce,
};
use lockstep_proto::{PROTOCOL_MAJOR, PROTOCOL_METADATA_KEY, PROTOCOL_MINOR};
use tonic::metadata::{Ascii, MetadataValue};
use tonic::transport::Channel;

use crate::rate::Pacer;
use crate::report::{BreachReport, Stats};
use crate::scenario::{Action, Breach, Outcome, RandomFlow, Rng, Single};

#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
enum CliSide {
    Buy,
    Sell,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
enum ScenarioName {
    /// Limit orders around a drifting mid, some crossing; ~20% cancels, ~5% market orders.
    Random,
    /// Trader 666 buys aggressively until blocked; exits non-zero if never blocked.
    Breach,
    /// Identical orders (the original behaviour).
    Single,
}

impl ScenarioName {
    fn label(self) -> &'static str {
        match self {
            Self::Random => "random",
            Self::Breach => "breach",
            Self::Single => "single",
        }
    }
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
    #[arg(long, value_enum, default_value = "random")]
    scenario: ScenarioName,
    /// Parallel workers, each with its own connection and request stream.
    #[arg(long, default_value_t = 1, value_parser = clap::value_parser!(u32).range(1..=4096))]
    concurrency: u32,
    /// Aggregate target rate in operations per second.
    #[arg(long, conflicts_with = "unlimited")]
    rate: Option<f64>,
    /// Send as fast as responses come back (the default).
    #[arg(long)]
    unlimited: bool,
    /// Run for this many seconds. Defaults: random 10, breach 30.
    #[arg(long, conflicts_with = "count")]
    duration: Option<u64>,
    /// Stop after this many operations. Default for `single`: 1.
    #[arg(long)]
    count: Option<u64>,
    /// Seed for the random scenario; same seed, same requests per worker.
    #[arg(long, default_value_t = 1)]
    seed: u64,
    /// Instruments the random scenario trades.
    #[arg(long, value_delimiter = ',', default_values_t = [1_u32, 2, 3, 4])]
    instruments: Vec<u32>,
    /// Number of distinct traders in the random scenario (ids start at --trader).
    #[arg(long, default_value_t = 4, value_parser = clap::value_parser!(u64).range(1..=20))]
    traders: u64,
    /// Trader id for `single`; first trader id for `random`.
    #[arg(long, default_value_t = 1)]
    trader: u64,
    /// One instrument for every scenario; overrides --instruments.
    #[arg(long)]
    instrument: Option<u32>,
    #[arg(long, value_enum, default_value = "buy")]
    side: CliSide,
    /// Limit price in ticks (`single` only).
    #[arg(long, default_value_t = 100)]
    price: i64,
    /// Order quantity (`single` only).
    #[arg(long, default_value_t = 10)]
    quantity: u64,
    /// Added to every client order id. Ids restart at 1 on each run, so use a
    /// different offset (or trader) when reusing an exchange that kept state.
    #[arg(long, default_value_t = 0)]
    client_id_offset: u64,
    /// Print one JSON object instead of the text summary.
    #[arg(long)]
    json: bool,
    /// Exit with an error unless every operation is accepted.
    #[arg(long)]
    expect_accepted: bool,
    /// Give up connecting after this long (the exchange may still be starting).
    #[arg(long, default_value = "10")]
    connect_timeout_secs: u64,
}

impl Args {
    fn instrument_list(&self) -> Vec<u32> {
        self.instrument
            .map_or_else(|| self.instruments.clone(), |one| vec![one])
    }
}

type Client = OrderEntryServiceClient<Channel>;

async fn connect(target: &str, timeout: Duration) -> anyhow::Result<Client> {
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

// ------------------------------------------------------------ sending

/// Back-pressure retries before an operation is given up on.
const MAX_RETRIES: u32 = 100;
const BACKOFF_START: Duration = Duration::from_millis(1);
const BACKOFF_MAX: Duration = Duration::from_millis(100);

/// The ack of a call, or the gRPC status that replaced it.
async fn call(
    client: &mut Client,
    version: &MetadataValue<Ascii>,
    action: &Action,
) -> Result<CommandAck, tonic::Status> {
    macro_rules! send {
        ($method:ident, $payload:expr) => {{
            let mut request = tonic::Request::new($payload.clone());
            request
                .metadata_mut()
                .insert(PROTOCOL_METADATA_KEY, version.clone());
            client
                .$method(request)
                .await?
                .into_inner()
                .ack
                .ok_or_else(|| tonic::Status::data_loss("response without an ack"))
        }};
    }
    match action {
        Action::Submit(order) => send!(submit_order, order),
        Action::Cancel(cancel) => send!(cancel_order, cancel),
    }
}

fn is_back_pressure(ack: &CommandAck) -> bool {
    matches!(&ack.result, Some(AckResult::Rejected(r)) if r.reason == RejectReason::Overloaded as i32)
}

/// Sends `action`, retrying back-pressure with jittered exponential back-off,
/// and records the final result in `stats`.
async fn execute(
    client: &mut Client,
    version: &MetadataValue<Ascii>,
    action: &Action,
    jitter: &mut Rng,
    deadline: Option<Instant>,
    stats: &mut Stats,
) -> Outcome {
    let mut backoff = BACKOFF_START;
    for _ in 0..=MAX_RETRIES {
        let started = Instant::now();
        let result = call(client, version, action).await;
        let latency = started.elapsed();
        match result {
            Ok(ack) if !is_back_pressure(&ack) => {
                stats.record_latency(latency);
                return match ack.result {
                    Some(AckResult::Accepted(ok)) => {
                        stats.accepted += 1;
                        Outcome::Accepted {
                            order_id: ok.order_id,
                        }
                    }
                    Some(AckResult::Rejected(rejection)) => {
                        stats.record_rejection(rejection.reason);
                        Outcome::Rejected(
                            RejectReason::try_from(rejection.reason)
                                .unwrap_or(RejectReason::Unspecified),
                        )
                    }
                    None => {
                        stats.errors += 1;
                        Outcome::Failed
                    }
                };
            }
            Ok(_) => stats.retries += 1,
            Err(failure) if failure.code() == tonic::Code::ResourceExhausted => stats.retries += 1,
            Err(failure) => {
                eprintln!("error: {failure}");
                stats.errors += 1;
                return Outcome::Failed;
            }
        }
        if deadline.is_some_and(|d| Instant::now() >= d) {
            break;
        }
        let half = backoff / 2;
        let pause = half + backoff.mul_f64(jitter_fraction(jitter)) / 2;
        tokio::time::sleep(pause).await;
        backoff = (backoff * 2).min(BACKOFF_MAX);
    }
    stats.resource_exhausted += 1;
    Outcome::Failed
}

/// Uniform in [0, 1).
fn jitter_fraction(rng: &mut Rng) -> f64 {
    #[expect(clippy::cast_precision_loss)]
    let fraction = rng.below(1 << 24) as f64 / f64::from(1_u32 << 24);
    fraction
}

// ------------------------------------------------------------ workers

enum Flow {
    Single(Single),
    Random(RandomFlow),
}

impl Flow {
    fn next(&mut self) -> Action {
        match self {
            Self::Single(flow) => flow.next(),
            Self::Random(flow) => flow.next(),
        }
    }

    fn observe(&mut self, outcome: Outcome) {
        if let Self::Random(flow) = self {
            flow.observe(outcome);
        }
    }
}

/// Run limits and pacing shared by all workers.
struct Shared {
    pacer: Pacer,
    deadline: Option<Instant>,
    count: Option<u64>,
    /// Operation tickets handed out so far.
    issued: AtomicU64,
    version: MetadataValue<Ascii>,
    client_id_offset: u64,
}

impl Shared {
    /// Applies `--client-id-offset`.
    fn offset(&self, mut action: Action) -> Action {
        if let Action::Submit(order) = &mut action {
            order.client_order_id += self.client_id_offset;
        }
        action
    }

    fn expired(&self) -> bool {
        self.deadline.is_some_and(|d| Instant::now() >= d)
    }

    /// True while this worker may start another operation.
    fn take_ticket(&self) -> bool {
        match self.count {
            // Relaxed: a plain ticket counter, it synchronises no other data.
            Some(limit) => self.issued.fetch_add(1, Ordering::Relaxed) < limit,
            None => true,
        }
    }
}

async fn worker(mut client: Client, mut flow: Flow, mut jitter: Rng, shared: Arc<Shared>) -> Stats {
    let mut stats = Stats::new();
    while !shared.expired() && shared.take_ticket() {
        shared.pacer.acquire().await;
        if shared.expired() {
            break;
        }
        let action = shared.offset(flow.next());
        let outcome = execute(
            &mut client,
            &shared.version,
            &action,
            &mut jitter,
            shared.deadline,
            &mut stats,
        )
        .await;
        flow.observe(outcome);
    }
    stats
}

/// Trader 666 against trader 1's liquidity until the exchange blocks 666.
async fn breach(
    mut client: Client,
    instrument_id: u32,
    shared: Arc<Shared>,
) -> (Stats, BreachReport) {
    let mut stats = Stats::new();
    let mut jitter = Rng::for_worker(0, u64::MAX);
    let mut flow = Breach::new(instrument_id);
    let mut first_fill: Option<Instant> = None;
    let mut report = BreachReport::default();
    while !shared.expired() && shared.take_ticket() {
        shared.pacer.acquire().await;
        let action = shared.offset(flow.next());
        let outcome = execute(
            &mut client,
            &shared.version,
            &action,
            &mut jitter,
            shared.deadline,
            &mut stats,
        )
        .await;
        if !flow.last_was_buy() {
            continue;
        }
        match outcome {
            // Acks carry no fills; an accepted buy against the liquidity just
            // offered is taken as the fill (market data is out of scope here).
            Outcome::Accepted { .. } => {
                first_fill.get_or_insert_with(Instant::now);
            }
            Outcome::Rejected(RejectReason::TraderBlocked) => {
                report.blocked = true;
                report.first_fill_to_block = first_fill.map(|t| t.elapsed());
                break;
            }
            Outcome::Rejected(_) | Outcome::Failed => {}
        }
    }
    (stats, report)
}

// ------------------------------------------------------------ main

fn single_template(args: &Args) -> SubmitOrderRequest {
    SubmitOrderRequest {
        trader_id: args.trader,
        client_order_id: 0,
        instrument_id: args.instrument.unwrap_or(1),
        side: match args.side {
            CliSide::Buy => Side::Buy,
            CliSide::Sell => Side::Sell,
        }
        .into(),
        r#type: OrderType::Limit.into(),
        time_in_force: TimeInForce::Gtc.into(),
        price_ticks: args.price,
        quantity: args.quantity,
    }
}

fn make_flow(args: &Args, index: u64) -> Flow {
    match args.scenario {
        ScenarioName::Single => Flow::Single(Single::new(index, single_template(args))),
        ScenarioName::Random | ScenarioName::Breach => Flow::Random(RandomFlow::new(
            args.seed,
            index,
            args.trader + index % args.traders,
            args.instrument_list(),
        )),
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    if args
        .rate
        .is_some_and(|rate| !(rate.is_finite() && rate > 0.0))
    {
        bail!("--rate must be a positive number");
    }
    if args.instrument_list().is_empty() {
        bail!("at least one instrument is required");
    }
    let version: MetadataValue<Ascii> = format!("{PROTOCOL_MAJOR}.{PROTOCOL_MINOR}").parse()?;

    let connect_timeout = Duration::from_secs(args.connect_timeout_secs);
    let first = connect(&args.target, connect_timeout).await?;

    let (count, duration) = match (args.count, args.duration, args.scenario) {
        (None, None, ScenarioName::Single) => (Some(1), None),
        (None, None, ScenarioName::Random) => (None, Some(10)),
        (None, None, ScenarioName::Breach) => (None, Some(30)),
        (count, duration, _) => (count, duration),
    };
    let started = Instant::now();
    let shared = Arc::new(Shared {
        pacer: Pacer::new(started, args.rate),
        deadline: duration.map(|secs| started + Duration::from_secs(secs)),
        count,
        issued: AtomicU64::new(0),
        version,
        client_id_offset: args.client_id_offset,
    });

    let (stats, breach_report) = if args.scenario == ScenarioName::Breach {
        let instrument = args.instrument_list()[0];
        let (stats, report) = breach(first, instrument, shared).await;
        (stats, Some(report))
    } else {
        let mut handles = Vec::new();
        let mut clients = vec![first];
        for _ in 1..args.concurrency {
            clients.push(connect(&args.target, connect_timeout).await?);
        }
        for (index, client) in (0_u64..).zip(clients) {
            let flow = make_flow(&args, index);
            let jitter = Rng::for_worker(args.seed ^ 0xA5A5_A5A5, index);
            handles.push(tokio::spawn(worker(client, flow, jitter, shared.clone())));
        }
        let mut total = Stats::new();
        for handle in handles {
            total.merge(&handle.await.context("worker panicked")?);
        }
        (total, None)
    };
    let elapsed = started.elapsed();

    let label = args.scenario.label();
    if args.json {
        println!("{}", report::to_json(label, &stats, elapsed, breach_report));
    } else {
        print!("{}", report::to_text(label, &stats, elapsed, breach_report));
    }

    if let Some(report) = breach_report
        && !report.blocked
    {
        bail!("trader 666 was never blocked within the run");
    }
    if args.expect_accepted {
        let bad = stats.rejected_total() + stats.resource_exhausted + stats.errors;
        if bad > 0 {
            bail!("{bad} operation(s) not accepted but --expect-accepted was given");
        }
    }
    Ok(())
}
