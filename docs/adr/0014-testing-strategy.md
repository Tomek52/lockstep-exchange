# 14. Testing strategy

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

The system combines pure logic (matching), concurrency (queues, runtime),
untrusted input (wire decoding, journal files), and cross-process integration
(two services, two languages). No single kind of test covers all of that.
Much of the code is written by LLM agents, so tests are also the main defence
against plausible-looking wrong code (see [ai-workflow.md](../ai-workflow.md)).

## Decision

| Level | What | Where | Runs in |
|---|---|---|---|
| **Unit** | Domain rules, codec mapping, queue contracts, journal format. One test executable per layer, linking only that layer. | `exchange-core/tests/<layer>/` | every preset |
| **Compile-time** | `static_assert`s for constexpr logic (validation boundaries, strong-type non-convertibility, journal header round trip, concept conformance). | alongside unit tests | every compile |
| **Architecture** | Include allow-lists per layer plus a self-test fixture (ADR-0002). | `tests/architecture/` | every preset |
| **Concurrency** | Multi-producer stress and contract tests; differential tests against `MutexQueue`. | `tests/concurrency/`, `tests/app/` | **tsan** (mandatory), all others |
| **Determinism** | Live multi-threaded run vs single-threaded journal replay, per shard (ADR-0004). | `tests/determinism/` | every preset, including tsan |
| **Property** | Randomised inputs with invariants: replay determinism (C++), PnL neutrality (Rust `proptest`). | as above | every run |
| **Fuzz** | libFuzzer on trust boundaries: order-entry decoding (skeleton), journal decoding (task 009), risk-command decoding (task 014). | `exchange-core/fuzz/` | asan-ubsan (smoke run in CTest), 60 s in CI |
| **In-process integration** | gRPC services over loopback against fake ports (C++); tonic Monitor handshake over loopback (Rust). | `tests/grpc/`, `rust/crates/risk-sentinel/tests/` | all but tsan (C++), cargo test (Rust) |
| **End-to-end** | Both services + loadgen as separate processes. | `scripts/e2e-smoke.sh` | CI `e2e` job |
| **Benchmarks** | Google Benchmark with per-operation p50/p99/p99.9 counters. | `exchange-core/bench/` | release preset, manually and before perf claims |

Conventions:

- **Labels:** exactly one CTest label per test executable (`unit`,
  `determinism`, `architecture`, `grpc`, `fuzz`). `grpc` is excluded from
  TSan (ADR-0007).
- **Test-first for new behaviour.** Task specs list acceptance tests; they
  are written, and seen failing, before the implementation (see
  ai-workflow.md).
- **Tests are not adjusted to pass.** A failing test is either a bug in the
  code or a wrong specification. The latter needs an explicit note in the PR.
  During the skeleton build, a Rust test asserting "extreme values do not
  overflow" failed. It turned out the *claim* was wrong (accumulation can
  overflow); the test was narrowed to the true property and the gap recorded
  in ADR-0005.
- **Benchmarks report distributions, not means.** Tail latency is what
  matters for a matching engine.

## Consequences

- Every layer can be tested in isolation, and the fast suites run in about a
  second.
- TSan meaningfully covers the concurrent core. gRPC adapters get ASan/UBSan
  plus integration tests instead.
- Fuzz corpora are not committed yet. CI starts from an empty corpus each
  run; a persisted corpus is a possible follow-up.
