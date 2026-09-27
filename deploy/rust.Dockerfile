# syntax=docker/dockerfile:1
# risk-sentinel and loadgen share one image (same workspace, same proto codegen).
# The builder tag matches rust/rust-toolchain.toml so rustup downloads nothing.
# Build context: repository root.

FROM rust:1.98.1-slim-trixie AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends protobuf-compiler \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY proto proto
COPY rust rust
WORKDIR /src/rust
RUN cargo build --release --locked -p risk-sentinel -p lockstep-loadgen

FROM debian:trixie-slim AS runtime
RUN useradd --system --no-create-home lockstep
COPY --from=build /src/rust/target/release/risk-sentinel /usr/local/bin/risk-sentinel
COPY --from=build /src/rust/target/release/loadgen /usr/local/bin/loadgen
USER lockstep
EXPOSE 50052
CMD ["risk-sentinel", "--listen", "0.0.0.0:50052"]
