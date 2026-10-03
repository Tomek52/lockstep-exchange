# 008: Journal record codec, CRC32C and file writer

## Goal

Persist every shard's commands to disk in the format of
[ADR-0012](../adr/0012-journal-binary-format.md), implementing the
`app::Journal` port. Replace `NullJournal` in `main`.

## Context

- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md):
  write-ahead semantics. `append()` is called before `apply()`; `commit()`
  once per batch, before outputs are released.
- [ADR-0012](../adr/0012-journal-binary-format.md): layout, CRC32C, command
  tags, max payload size.
- Existing pieces:
  - `exchange-core/adapters/journal/include/lockstep/journal/format.hpp`
    (FileHeader, RecordHeader, encode/decode);
  - `byte_io.hpp` (`store_le`/`load_le`, constexpr via `if consteval`);
  - `memory_journal.hpp`.
- Port: `exchange-core/app/include/lockstep/app/ports/journal.hpp`.
- The journal adapter must not link protobuf or gRPC (so the determinism
  tests stay TSan-clean, ADR-0007). Enforced by `lockstep_restrict_links` and
  `ctest -L architecture`.
- Commands: `exchange-core/domain/include/lockstep/domain/commands.hpp`. Use
  `CommandTag`, never `variant::index()`.
- **Note (task 004):** `RiskLinkPolicy` (FailOpen/FailClosed) changes
  `ShardEngine`'s output, so `config_hash` covers the whole
  output-relevant `ShardConfig` (instrument specs and the policy), as decided
  in [ADR-0017](../adr/0017-journal-config-hash-covers-shard-config.md#adr0017-decision-v1),
  which supersedes that part of ADR-0012.

## Interfaces to implement

```cpp
// lockstep/journal/crc32c.hpp
[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> data,
                                             std::uint32_t seed = 0) noexcept; // table-driven, constexpr

// lockstep/journal/record_codec.hpp
/// Appends the payload encoding of `command` to `out` (no header).
void encode_payload(const domain::SequencedCommand& command, std::vector<std::byte>& out);
/// Decodes one payload. Total: never UB on arbitrary bytes.
[[nodiscard]] std::expected<domain::SequencedCommand, app::JournalError>
decode_payload(std::span<const std::byte> payload) noexcept;

/// FNV-1a of the shard's output-relevant config for FileHeader::config_hash (ADR-0017).
[[nodiscard]] constexpr std::uint64_t config_hash(const domain::ShardConfig& config) noexcept;

// lockstep/journal/file_journal_writer.hpp
enum class SyncPolicy : std::uint8_t { None, EveryCommit };
class FileJournalWriter final : public app::Journal {
public:
    /// Creates <dir>/shard-<id>.jnl with a fresh header. Throws std::runtime_error
    /// if the file cannot be created or already exists (startup error, ADR-0008).
    static std::unique_ptr<FileJournalWriter> create(const std::filesystem::path& dir,
        const journal::FileHeader& header, SyncPolicy policy);
    std::expected<void, app::JournalError> append(const domain::SequencedCommand&) override; // buffers
    std::expected<void, app::JournalError> commit() override; // write(2) the buffer, fdatasync per policy
};
```

- **Payload:** `sequence u64 | timestamp i64 | tag u8 | fields`, all
  little-endian through `byte_io`. Enums are one byte each; bools are one
  byte (0/1; anything else is `Corrupt` on decode).
- **Buffering:** `append` never does I/O. `commit` writes all buffered
  records with as few `write` calls as possible, and handles short writes.
- **`main`:** add `--journal-dir=DIR` (default `./journal`) and
  `--fsync=none|commit` (default `commit`). Build the header with the shard
  id, shard count and `config_hash` of the shard's config (instruments from
  `Router::instruments_of(shard)` plus the risk link policy). Delete the NullJournal warning.

## Acceptance criteria

1. `crc32c` of the ASCII bytes `"123456789"` is `0xE3069283`, as a
   `static_assert`.
2. **Round trip** for every `Command` alternative with non-trivial field
   values: `decode_payload(encode_payload(x)) == x`.
3. **Golden bytes:** one hand-written byte array per command tag is pinned in
   a test, so accidental format changes fail loudly.
4. **Decoder totality:** truncated payloads, unknown tags, invalid enum
   bytes, and bools not in {0,1} each return `Corrupt`. No crash under
   asan-ubsan.
5. **Writer:** write 1 000 commands in 10 commits. The file size equals
   `file_header_size` plus the sum of (`record_header_size` + payload). The
   header decodes to what was written, and each record's CRC matches.
6. **Main:** the e2e smoke test passes and leaves `shard-0.jnl` and
   `shard-1.jnl` in the journal directory. The script uses a temp dir and
   cleans it up.
7. The `lockstep_journal` target becomes STATIC (add `src/*.cpp`) and still
   links only `lockstep::app`. `ctest -L architecture` passes.
8. Presets debug, asan-ubsan and tsan pass; clang-tidy is clean.

## Files expected to change

- `exchange-core/adapters/journal/include/lockstep/journal/{crc32c.hpp,record_codec.hpp,file_journal_writer.hpp}` (new)
- `exchange-core/adapters/journal/src/{record_codec.cpp,file_journal_writer.cpp}` (new)
- `exchange-core/adapters/journal/CMakeLists.txt`
- `exchange-core/tests/journal/*` (new test files) and `tests/journal/CMakeLists.txt`
- `exchange-core/main/src/{main.cpp,options.hpp,options.cpp}`
- `scripts/e2e-smoke.sh` (journal dir)

## Out of scope

- Reading and recovery (task 009). Replay tooling (task 010).
- Rotation, compaction, snapshots.

## Dependencies

None. Task 009 depends on this one.
