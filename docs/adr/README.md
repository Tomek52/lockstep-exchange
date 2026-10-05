# Architecture Decision Records

Decisions that shape the system, in [Michael Nygard's format](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions):
**Context → Decision → Consequences**. An ADR is never edited to change its
decision; a new ADR supersedes it and the old one's status says so.

| #    | Decision | Status |
|------|----------|--------|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted |
| [0002](0002-hexagonal-architecture-enforced-by-the-build.md) | Hexagonal architecture, enforced by the build | Accepted |
| [0003](0003-single-writer-sharding.md) | Single-writer sharding with per-shard ingress and a single publisher | Accepted |
| [0004](0004-deterministic-replay-via-per-shard-journal.md) | Deterministic replay via a per-shard write-ahead journal | Accepted |
| [0005](0005-fixed-point-prices-and-quantities.md) | Fixed-point prices and integer quantities | Accepted |
| [0006](0006-grpc-service-and-stream-design.md) | gRPC service and stream design, protocol versioning | Accepted |
| [0007](0007-dependency-management-system-packages.md) | Dependencies from Ubuntu 24.04 packages, not vcpkg | Accepted |
| [0008](0008-error-handling-strategy.md) | Error handling: `std::expected` inside, exceptions at the edges | Accepted |
| [0009](0009-toolchain-baseline-and-feature-fallbacks.md) | Toolchain baseline and C++23 feature fallbacks | Accepted |
| [0010](0010-no-cpp20-modules-for-now.md) | No C++20 modules (for now) | Accepted |
| [0011](0011-lock-free-queues-and-memory-ordering.md) | Lock-free queue designs and memory-ordering policy | Accepted |
| [0012](0012-journal-binary-format.md) | Own binary journal format, not protobuf | Accepted (`config_hash` superseded by 0017) |
| [0013](0013-risk-feedback-loop.md) | Risk feedback loop semantics | Accepted |
| [0014](0014-testing-strategy.md) | Testing strategy | Accepted |
| [0015](0015-documentation-site-and-versioned-anchors.md) | Documentation site (MkDocs) and versioned requirement anchors | Proposed |
| [0016](0016-agent-context-budget.md) | Agent context budget: no duplicate loading, quiet logs, path-scoped rules | Proposed |
| [0017](0017-journal-config-hash-covers-shard-config.md) | Journal `config_hash` covers the whole output-relevant shard config | Accepted |
| [0018](0018-sentinel-overflow-policy.md) | Sentinel accumulated-cash overflow policy | Accepted |
| [0019](0019-configuration-format.md) | Configuration format (JSON, nlohmann) and parser | Accepted |
| [0020](0020-journal-restart-recover-replay-resume.md) | Journal restart: recover, replay, resume appending | Accepted |

To add one: copy the structure of any ADR, take the next number, add it to
this table, and link it from the code or task spec that motivated it.
