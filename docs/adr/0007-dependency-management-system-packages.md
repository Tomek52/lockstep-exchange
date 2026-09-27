# 7. Dependencies from Ubuntu 24.04 packages, not vcpkg

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

exchange-core needs gRPC, protobuf, GoogleTest and Google Benchmark. We need
the setup to be:

- **fast** on a fresh machine and in every CI job (a few minutes, not tens);
- **reproducible**: CI, Docker and developer machines resolve the same
  versions;
- **sanitizer-compatible**: TSan is mandatory in CI (ADR-0003).

Options considered:

| Option | Setup time (fresh) | Reproducibility | TSan-instrumented gRPC |
|---|---|---|---|
| **vcpkg manifest mode** | 20–40 min to build gRPC + abseil + protobuf from source per triplet, unless a binary cache is set up and maintained | Excellent (baseline commit pins everything) | Possible with a custom TSan triplet: another full gRPC build |
| **FetchContent / submodules** | Same source build cost, inside every build tree | Good | Possible, same cost |
| **Ubuntu 24.04 apt packages** | ~2–3 min | Good: one distro release pins versions (gRPC 1.51.1, protobuf 3.21.12, GTest 1.14, Benchmark 1.8.3) | No |

## Decision

Use **Ubuntu 24.04 system packages** for all C++ dependencies, installed by
`scripts/setup-ubuntu.sh` (idempotent; used by developers, CI and the
Dockerfile). Also installed by that script:

- GCC 14 and Clang 19 from the Ubuntu archive (no third-party PPA or
  apt.llvm.org needed; verified when the skeleton was built);
- `buf`, pinned by version and SHA-256-verified;
- Rust via rustup, pinned by `rust/rust-toolchain.toml`.

**TSan consequence, stated explicitly.** apt's gRPC and protobuf are not
TSan-instrumented. We observed the expected false positives while building
the skeleton: TSan reports data races inside `epoll_wait` and `memmove` in
gRPC's I/O threads. Therefore:

- every test executable that links gRPC or protobuf carries the CTest label
  `grpc`, and the `tsan` test preset excludes that label;
- everything that matters for thread safety stays gRPC-free *by
  architecture* (ADR-0002), so it runs fully instrumented: domain,
  concurrency, the app runtime, the journal and the determinism tests;
- gRPC adapters are still covered by ASan+UBSan (which work with
  uninstrumented libraries) and by the e2e smoke test.

`lockstep_add_test` allows exactly one label per test executable. CMake 3.28's
`gtest_discover_tests` flattens list-valued labels, which once silently
dropped the `grpc` label and let gRPC tests into the TSan run.

## Consequences

- Fresh setup measured in a clean `ubuntu:24.04` container: **160 s** for
  `scripts/setup-ubuntu.sh --ci` (apt packages, buf, rustup and the pinned
  toolchain), dominated by downloads.
- Versions move only when we move to a new Ubuntu LTS: a deliberate, reviewed
  upgrade.
- gRPC 1.51 is older than upstream. The callback API and everything else we
  use are available, and generated headers are included as SYSTEM so their
  warnings don't fail our `-Werror` build.
- Portability is "Ubuntu 24.04 (or its Docker image)". Other distros are best
  effort. If macOS/Windows-native builds ever matter, revisit with vcpkg plus
  a binary cache (new ADR).
