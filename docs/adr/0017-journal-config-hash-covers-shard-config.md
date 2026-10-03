# 17. Journal `config_hash` covers the whole output-relevant shard config

- **Status:** Accepted
- **Date:** 2026-10-03
- **Supersedes:** the `config_hash` bullet of [ADR-0012](0012-journal-binary-format.md)

## Context

ADR-0012 defines `FileHeader::config_hash` as a hash of the shard's
`InstrumentSpec`s, so that replay refuses a journal recorded under a different
configuration. Task 004 added `domain::RiskLinkPolicy` (FailOpen/FailClosed)
to `ShardConfig`. The policy changes `ShardEngine`'s output: under FailClosed,
new orders are rejected while the risk link is down. A journal replayed under
the other policy would therefore diverge silently, which breaks the
determinism property of [ADR-0004](0004-deterministic-replay-via-per-shard-journal.md#adr0004-determinism-property-v1).

## <a id="adr0017-decision-v1"></a>Decision

- `config_hash` is a 64-bit **FNV-1a** hash over a canonical little-endian
  encoding of every `domain::ShardConfig` input that affects `ShardEngine`
  output:
  1. the number of instrument specs (u32);
  2. each spec, sorted by id: `id u32 | min_price i64 | max_price i64 |
     max_order_quantity u64`;
  3. `risk_link_policy` as one byte with explicit values (FailOpen = 0,
     FailClosed = 1), independent of the enum's underlying values.
- `shard_id` and `shard_count` are not hashed; they have their own header
  fields and are checked separately.
- Signature: `config_hash(const domain::ShardConfig&)` in
  `lockstep/journal/record_codec.hpp`, `constexpr`, so a golden value can be
  pinned in a test.
- <a id="adr0017-new-fields-hashed-v1"></a>Any new `ShardConfig` field that
  affects output must be added to the hash. Any change to what is hashed or
  how is a format change and bumps `format_version` (ADR-0012).
- `format_version` stays 1: the header layout is unchanged, and no file
  journal had been written before this decision (only `MemoryJournal` and
  `NullJournal` existed).

FNV-1a is chosen because it is tiny, `constexpr`, identical on every platform
and needs no dependency. It guards against accidental configuration mismatch,
not against tampering; CRC32C already covers corruption. Rejected: CRC32C
(32 bits only), `std::hash` (not stable across implementations), SHA-256 (a
dependency for no benefit here).

## Consequences

- Replay detects a policy mismatch as `JournalError::ConfigMismatch`, as it
  already does for instrument specs.
- Adding a behaviour-changing field to `ShardConfig` now has a checklist item:
  hash it and bump the format version.
