# 19. Configuration format and parser

- **Status:** Accepted
- **Date:** 2026-10-04

## Context

Until now exchange-core took its instrument set from `--instruments=1,2,3,4`,
which lists ids only and leaves every `domain::InstrumentSpec` at its default
limits ([task 012](../tasks/012-instrument-config.md)). The exchange needs a
richer source of reference data: per-instrument price and quantity limits,
display metadata (symbol, tick and lot size, which are edge-only data per
[ADR-0005](0005-fixed-point-prices-and-quantities.md)), the shard count, and
the risk link policy ([ADR-0013](0013-risk-feedback-loop.md)).

A command line of comma-separated tuples would be unreadable and impossible to
validate well. We want a file. The requirements for the format and its parser:

- **A parser already in Ubuntu 24.04 apt**, header-only, with no build-time
  code generation and no new link dependency, to stay within
  [ADR-0007](0007-dependency-management-system-packages.md) (system packages,
  fast setup, no source builds).
- **Strict parsing**: an unknown key, a duplicate instrument id, or an
  out-of-range limit must fail loudly at startup, not be silently ignored.
  The journal header pins a hash of the shard config
  ([ADR-0012](0012-journal-binary-format.md),
  [ADR-0017](0017-journal-config-hash-covers-shard-config.md)), so the config
  the operator wrote and the config the engine runs must be the same bytes.
- **No floating point for money.** Tick and lot sizes are display strings;
  prices and quantities stay integer ticks and lots (ADR-0005).

Formats considered:

| Format | apt, header-only, no codegen | Strictness (reject unknown keys) | Comments | Fit |
|---|---|---|---|---|
| **JSON (nlohmann)** | `nlohmann-json3-dev`, header-only, no codegen | yes, by iterating keys and rejecting unknowns ourselves | no | good |
| TOML (`toml11`/`toml++`) | not both header-only *and* packaged in 24.04 at a version we rely on | yes | yes | adds a dependency outside apt |
| YAML (`libyaml`/`yaml-cpp`) | `libyaml-cpp-dev` exists but links a compiled library | manual | yes | compiled dep, surprising type coercions |
| Hand-rolled parser | nothing to install | whatever we build | – | parser code to own and fuzz, no reason to |

## Decision

Use **JSON**, parsed by **nlohmann-json** (`nlohmann-json3-dev`, 3.11.3 in
Ubuntu 24.04, header-only). It is already in apt, adds no link dependency and
no code generation, and the development team reads JSON fluently.

A new adapter library `lockstep::config`
(`exchange-core/adapters/config`) owns the schema and the parse. It exposes
`load_config(path)` and `parse_config(json)` returning
`std::expected<ExchangeConfig, std::string>`, where the error names the
offending field (ADR-0008: `std::expected` inside, no exceptions across the
boundary). The adapter links only `lockstep::domain` plus the header-only JSON
library, enforced by `lockstep_restrict_links` (ADR-0002).

**Strictness is explicit and tested.** `parse_config` rejects:

- unknown top-level or per-instrument keys (typos fail);
- duplicate instrument ids (they would hash differently from what
  `ShardEngine::try_emplace` keeps, ADR-0017);
- `min_price_ticks < 1`, `min > max`, a zero quantity limit;
- an unknown `risk_link_policy`;
- `shards < 1`.

Money stays integer: `min_price_ticks`, `max_price_ticks` and
`max_order_quantity` are JSON integers mapped to the domain's strong types;
`tick_size` and `lot_size` are JSON strings kept as display-only
`InstrumentMetadata`, never parsed into a `double`.

`--config=FILE` is added to the composition root. The legacy `--instruments`
and the per-flag defaults still work when no config is given, and passing both
`--config` and `--instruments` is an error (one source of truth at a time).

## Consequences

- One new apt package, `nlohmann-json3-dev`, added to
  `scripts/setup-ubuntu.sh` and `deploy/exchange-core.Dockerfile` (ADR-0007).
  It is header-only, so there is no runtime library to ship.
- The config is parsed once at startup; there is no hot reload (out of scope).
- nlohmann-json throws on malformed JSON; the adapter catches at its own
  boundary and converts to the `std::expected` error string, so no exception
  escapes into the app or domain (ADR-0008).
- JSON has no comments. If operators need annotated configs later, that is a
  reason to revisit (a new ADR), not to hand-roll a format now.
- This ADR takes number 0019: the task spec drafted it as "ADR-0015", a number
  since taken by the documentation-site ADR; the next free number is used per
  the index rule in [docs/adr/README.md](README.md).
