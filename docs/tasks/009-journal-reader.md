# 009: Lazy journal reader, recovery and journal fuzzer

## Goal

Read a shard journal back as a lazy sequence of commands using
`std::generator`, distinguish torn tails from corruption, provide tail
recovery, and fuzz the decoder.

## Context

- [ADR-0012](../adr/0012-journal-binary-format.md):
  - a record extending past EOF is `Truncated` (torn tail, recoverable);
  - a CRC mismatch or an impossible length before the tail is `Corrupt`
    (fatal);
  - a header version mismatch is `VersionMismatch`;
  - a config mismatch is `ConfigMismatch`.
- Task 008 provides `decode_payload`, `crc32c`, `config_hash`, the header
  codec and `FileJournalWriter`.
- `app::replay()` (`exchange-core/app/include/lockstep/app/replay.hpp`)
  accepts any input range of `SequencedCommand`, including the generator
  after a `transform`.
- Fuzz targets live in `exchange-core/fuzz/` (Clang + asan-ubsan preset).
  See `order_entry_decode_fuzzer.cpp` for the pattern and CTest smoke
  registration.

## Interfaces to implement

```cpp
// lockstep/journal/file_journal_reader.hpp
struct ReaderExpectations {           // what the caller requires of the file
    std::optional<domain::ShardId> shard;
    std::optional<std::uint32_t> shard_count;
    std::optional<std::uint64_t> config_hash;
};

/// Opens and validates the header, then yields records lazily. After the
/// first error it yields that error once and finishes. A torn tail yields
/// JournalError::Truncated as the last element.
[[nodiscard]] std::generator<std::expected<domain::SequencedCommand, app::JournalError>>
read_journal(std::filesystem::path path, ReaderExpectations expect = {});

/// Truncates a journal whose tail is torn to the end of its last complete,
/// CRC-valid record. Returns the number of valid records.
/// Returns an error for Corrupt/VersionMismatch (never modifies such files).
[[nodiscard]] std::expected<std::uint64_t, app::JournalError>
recover_tail(const std::filesystem::path& path);
```

Also add a CLI tool, `journal-dump` (target in `exchange-core/main`,
`src/journal_dump.cpp`). It prints the header and one line per record via
`std::print`, for example
`seq=12 ts=... NewOrder trader=1 instrument=2 Buy Limit 100@10`.

## Acceptance criteria

Tests in `tests/journal/file_journal_reader_test.cpp`, using
`FileJournalWriter` to produce inputs in a temp directory:

1. **Round trip:** write 10 000 commands, then read them back equal and in
   order. The generator is consumed lazily: reading the first element of a
   1-million-record file does not read the whole file. Assert via a
   file-size bound on bytes read, or by timing with a generous margin.
2. **Torn tail:** truncate the file inside the last record. The reader yields
   all complete records, then `Truncated` once. `recover_tail` returns the
   count, and reading again ends cleanly with no error.
3. **Corruption:** flip one byte inside the payload of record 5 of 10. The
   reader yields records 1–4, then `Corrupt`. `recover_tail` refuses with
   `Corrupt` and leaves the file unchanged.
4. **Header:** wrong magic → `Corrupt`; version + 1 → `VersionMismatch`;
   `ReaderExpectations` mismatch → `ConfigMismatch`.
5. **Fuzzer** `exchange-core/fuzz/journal_decode_fuzzer.cpp`: feeds arbitrary
   bytes to `decode_payload` and also to a record-sequence parser over an
   in-memory buffer. Registered as a CTest smoke test (label `fuzz`, 20 000
   runs). Passes under asan-ubsan.
6. `journal-dump` on a file from criterion 1 prints 10 000 lines plus a
   header line.
7. Presets debug, asan-ubsan and tsan pass; clang-tidy is clean.

## Files expected to change

- `exchange-core/adapters/journal/include/lockstep/journal/file_journal_reader.hpp` (new)
- `exchange-core/adapters/journal/src/file_journal_reader.cpp` (new), `adapters/journal/CMakeLists.txt`
- `exchange-core/fuzz/journal_decode_fuzzer.cpp` (new), `exchange-core/fuzz/CMakeLists.txt`
- `exchange-core/main/src/journal_dump.cpp` (new), `exchange-core/main/CMakeLists.txt`
- `exchange-core/tests/journal/file_journal_reader_test.cpp` (new), `tests/journal/CMakeLists.txt`

## Out of scope

- Replaying into engines and comparing outputs (task 010).
- Resuming writes after recovery (task 010 decides whether `main` resumes or
  starts a new generation).

## Dependencies

- **Hard:** 008.
