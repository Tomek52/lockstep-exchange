# 12. Own binary journal format, not protobuf

- **Status:** Accepted; the `config_hash` definition is superseded by [ADR-0017](0017-journal-config-hash-covers-shard-config.md)
- **Date:** 2026-09-27

## Context

The journal (ADR-0004) is written on every shard thread for every command and
read back for replay and recovery. Requirements:

- cheap to encode on the hot path, with no allocation;
- robust to torn writes at the tail after a crash;
- detects corruption anywhere else;
- versioned;
- testable under ThreadSanitizer with fully instrumented code (ADR-0007),
  which rules out linking uninstrumented protobuf into the journal and
  determinism tests.

Using the protobuf messages we already have would be convenient, but it would
pull protobuf into the journal adapter and the determinism tests. It would
also tie the storage format to the wire format, which evolves for different
reasons.

## Decision

A small, explicit, little-endian binary format owned by
`adapters/journal/include/lockstep/journal/format.hpp`:

```
file   := FileHeader Record*
FileHeader (32 B): magic "LKSTPJNL" | version u16 | reserved u16 | shard_id u32
                   | shard_count u32 | reserved u32 | config_hash u64
Record: payload_size u32 | crc32c u32 | payload[payload_size]
payload: SequencedCommand = sequence u64 | timestamp i64 | tag u8 | fields...
```

- **Command tags** are the explicit `domain::CommandTag` values, never
  `std::variant::index()`. Reordering the variant alternatives cannot change
  the format. Tags are append-only.
- **CRC32C** covers each payload.
  - A record whose header or payload extends past end-of-file is
    **Truncated**: a torn tail write. The reader stops there, and recovery
    truncates the file to the last good record.
  - A CRC mismatch before the tail is **Corrupt**, and fatal.
- **`config_hash`** is a hash of the shard's `InstrumentSpec`s. Together with
  `shard_id`/`shard_count`, replay refuses a journal produced under a
  different configuration (`JournalError::ConfigMismatch`).
- **Byte I/O** (`byte_io.hpp`) uses `if consteval`: shift-based at compile
  time, so header round trips are `static_assert`s, and `memcpy` at run time.
- **Max payload size** is 4 KiB. Anything larger is corruption, which bounds
  the damage a fuzzer or bit flip can do.
- **Readers** yield records lazily through `std::generator` (task 009).

## Consequences

- The journal and all determinism tests stay protobuf-free and run under
  TSan.
- We own a codec, which needs its own tests: round trips, golden bytes, and a
  libFuzzer target for the decoder (task 009).
- Format changes bump `format_version`. Old readers reject new files
  explicitly (`VersionMismatch`) instead of misreading them.
