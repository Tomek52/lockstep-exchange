# 012: Instrument reference data and configuration file

## Goal

Replace `--instruments=1,2,3,4`, which only lists ids and takes the
`InstrumentSpec` defaults, with a configuration file that describes each
instrument's limits and display metadata and the engine settings. Record the
new dependency in an ADR.

## Context

- `domain::InstrumentSpec` (`exchange-core/domain/include/lockstep/domain/types.hpp`)
  has `id`, `min_price`, `max_price` and `max_order_quantity`. Validation
  (`validation.hpp`) uses them.
- [ADR-0005](../adr/0005-fixed-point-prices-and-quantities.md): tick and lot
  sizes are reference data used only at the edges, for display and
  conversion. The domain works in ticks and lots.
- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md) and
  [ADR-0012](../adr/0012-journal-binary-format.md): the journal header pins a
  hash of the shard's specs, so they must come from a reproducible source.
- [ADR-0017](../adr/0017-journal-config-hash-covers-shard-config.md):
  `config_hash` folds in every instrument spec, while `ShardEngine` keeps
  only the first occurrence of a given id (`try_emplace`). A config with
  duplicate instrument ids would therefore hash differently from what the
  engine actually runs, so `load_config`/`parse_config` must reject
  duplicates rather than silently drop them.
- [ADR-0007](../adr/0007-dependency-management-system-packages.md): new
  dependencies come from Ubuntu 24.04 apt, and must be added to
  `scripts/setup-ubuntu.sh` and `deploy/exchange-core.Dockerfile`.
- Composition root: `exchange-core/main/src/{main.cpp,options.*}`.

## Interfaces to implement

Example config file `config/exchange.json`:

```json
{
  "shards": 2,
  "risk_link_policy": "fail_open",
  "instruments": [
    { "id": 1, "symbol": "LKS-A", "tick_size": "0.01", "lot_size": "1",
      "min_price_ticks": 1, "max_price_ticks": 100000000, "max_order_quantity": 100000 }
  ]
}
```

New adapter library `lockstep::config` (`exchange-core/adapters/config`):

```cpp
struct InstrumentMetadata { domain::InstrumentId id; std::string symbol;
                            std::string tick_size; std::string lot_size; };  // display only
struct ExchangeConfig {
    std::size_t shards;
    domain::RiskLinkPolicy risk_link_policy;
    std::vector<domain::InstrumentSpec> instruments;
    std::vector<InstrumentMetadata> metadata;
};
/// Parses and validates; the error names the offending field.
[[nodiscard]] std::expected<ExchangeConfig, std::string> load_config(const std::filesystem::path& file);
[[nodiscard]] std::expected<ExchangeConfig, std::string> parse_config(std::string_view json);
```

Validation errors:

- duplicate instrument ids;
- `min_price_ticks < 1` or `min > max`;
- zero quantity limit;
- unknown policy;
- `shards < 1`;
- unknown keys (be strict: typos should fail).

`main` gains `--config=FILE`. Existing flags still work when no config is
given, and `--config` together with `--instruments` is an error. Wire
`risk_link_policy` into `EngineConfig`.

**JSON library:** use `nlohmann-json3-dev` (apt, header-only). Write
**ADR-0015**, "Configuration format and parser": why JSON over TOML/YAML or
a hand-rolled format, and why nlohmann (in apt, header-only, no build cost).
Add it to the ADR index.

## Acceptance criteria

1. **Unit tests** in `exchange-core/tests/config/config_test.cpp`: a valid
   file parses to the expected `ExchangeConfig`, and each validation error
   above has a test asserting the message mentions the field.
2. `config/exchange.json` exists, is used by `deploy/docker-compose.yml`
   (mounted or copied), and the compose stack still starts.
3. `scripts/setup-ubuntu.sh` and `deploy/exchange-core.Dockerfile` install
   `nlohmann-json3-dev`.
4. `lockstep::config` links only `lockstep::domain` (plus the header-only JSON
   library). Add a `lockstep_restrict_links` rule.
5. `docs/adr/0015-*.md` exists and is indexed in `docs/adr/README.md`.
6. The e2e smoke test passes, both with and without `--config`.
7. Presets debug, asan-ubsan and tsan pass; clang-tidy is clean.

## Files expected to change

- `exchange-core/adapters/config/**` (new), `exchange-core/adapters/CMakeLists.txt`
- `exchange-core/tests/config/**` (new), `exchange-core/tests/CMakeLists.txt` (`add_subdirectory(config)`)
- `exchange-core/main/src/{main.cpp,options.hpp,options.cpp}`, `exchange-core/main/CMakeLists.txt`
- `config/exchange.json` (new)
- `scripts/setup-ubuntu.sh`, `deploy/exchange-core.Dockerfile`, `deploy/docker-compose.yml`
- `docs/adr/0015-configuration-format.md` (new), `docs/adr/README.md`

## Out of scope

- Hot reload. Per-instrument shard pinning (assignment stays round-robin by
  id, ADR-0003).

## Dependencies

None. Task 008 also edits `main.cpp` and `options.*`, in different options.
