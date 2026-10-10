//! risk-sentinel: consumes execution reports from exchange-core over a
//! bidirectional gRPC stream, keeps positions and PnL per trader, and sends
//! risk commands (block trader, kill switch) back when limits are breached.
//!
//! Layout mirrors the C++ side's ports-and-adapters split on a small scale:
//! `positions`, `limits` and `engine` are pure (no I/O, no async) and unit
//! tested; `session` is the Monitor message handling (no sockets, unit tested);
//! `service` is the tonic adapter and owns the stream and its timers.

pub mod engine;
pub mod limits;
pub mod positions;
pub mod service;
pub mod session;
