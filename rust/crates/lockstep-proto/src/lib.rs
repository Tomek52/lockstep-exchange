//! Generated gRPC/protobuf bindings for the `lockstep.v1` contracts, shared by
//! risk-sentinel (server) and the load generator (client).

/// Protocol major version: equals the proto package suffix (`v1`).
pub const PROTOCOL_MAJOR: u32 = 1;
/// Protocol minor version advertised by this build.
pub const PROTOCOL_MINOR: u32 = 0;
/// Metadata header that carries `"<major>.<minor>"` on unary calls.
pub const PROTOCOL_METADATA_KEY: &str = "x-lockstep-protocol";

/// Messages and services of package `lockstep.v1`.
pub mod v1 {
    #![allow(clippy::all, clippy::pedantic, unreachable_pub)]
    tonic::include_proto!("lockstep.v1");
}

/// The protocol version this build speaks, as a message.
#[must_use]
pub fn protocol_version() -> v1::ProtocolVersion {
    v1::ProtocolVersion {
        major: PROTOCOL_MAJOR,
        minor: PROTOCOL_MINOR,
    }
}
