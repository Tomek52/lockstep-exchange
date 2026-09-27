//! Compiles the shared contracts in `<repo>/proto` - the same files the C++
//! build compiles with protoc (ADR-0006). Requires `protoc` on PATH
//! (installed by scripts/setup-ubuntu.sh and the Docker images).

use std::path::PathBuf;

const PROTOS: [&str; 4] = [
    "lockstep/v1/common.proto",
    "lockstep/v1/order_entry.proto",
    "lockstep/v1/market_data.proto",
    "lockstep/v1/risk.proto",
];

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../proto");
    let files: Vec<PathBuf> = PROTOS.iter().map(|p| root.join(p)).collect();

    tonic_prost_build::configure()
        .build_server(true)
        .build_client(true)
        .compile_protos(&files, std::slice::from_ref(&root))?;

    for file in &files {
        println!("cargo:rerun-if-changed={}", file.display());
    }
    Ok(())
}
