# syntax=docker/dockerfile:1
# exchange-core: built with the same Ubuntu 24.04 system packages as CI and
# local development (ADR-0007), so the image and the dev box agree on versions.
# Build context: repository root (see deploy/docker-compose.yml).

FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++-14 cmake ninja-build ca-certificates \
      libgrpc++-dev libgrpc-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY proto proto
COPY exchange-core exchange-core
RUN cmake --preset release -DLOCKSTEP_BUILD_TESTS=OFF -DLOCKSTEP_BUILD_BENCHMARKS=OFF \
 && cmake --build --preset release --target exchange-core \
 && strip build/release/exchange-core/main/exchange-core

FROM ubuntu:24.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends libgrpc++1.51t64 libprotobuf32t64 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --no-create-home lockstep
COPY --from=build /src/build/release/exchange-core/main/exchange-core /usr/local/bin/exchange-core
USER lockstep
EXPOSE 50051
ENTRYPOINT ["exchange-core"]
CMD ["--listen=0.0.0.0:50051", "--risk-sentinel=risk-sentinel:50052"]
