# 010: File-based replay tool and the full determinism suite

## Goal

Turn deterministic replay from an in-memory test into a user-facing property:

- a tool that replays on-disk journals and prints a verifiable digest of
  every shard's outputs and books;
- a determinism test suite that covers matching, cancels, modifies, risk
  commands and the file journal.

## Context

- [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md) states
  the property and the rules. Determinism is per shard.
- Today, `tests/determinism/replay_determinism_test.cpp` checks the property
  with `MemoryJournal` and a generator that mostly produces
  `OrderAccepted` events, because matching did not exist when it was written.
- Building blocks:
  - `app::replay(ShardEngine&, range)` in `app/include/lockstep/app/replay.hpp`;
  - `app::Router::round_robin`;
  - `journal::read_journal` (task 009);
  - `journal::FileJournalWriter` (task 008);
  - `OrderBook::snapshot()` / `BookSnapshot`.
- Matching comes from task 002 (hard dependency). Modify and risk semantics
  come from 003/004 (soft: include them in the generator if they are merged,
  and note it in the PR if not).

## Interfaces to implement

```cpp
// app/include/lockstep/app/digest.hpp
/// Order-sensitive 64-bit digest (e.g. FNV-1a over a canonical byte encoding)
/// of events, replies and final book snapshots. Canonical = independent of
/// padding and platform endianness.
[[nodiscard]] std::uint64_t digest(const ReplayOutput& output,
                                   std::span<const domain::BookSnapshot> books);
```

Tool `lockstep-replay` (new target in `exchange-core/main`):

```
lockstep-replay --journal-dir=DIR --instruments=1,2,3,4 --shards=2
  shard 0: 12345 commands, 20211 events, digest=0x3f2a...
  shard 1: ...
```

It exits non-zero on any journal error other than a torn tail, which it
reports and treats as end of input. It must use the same instrument → shard
assignment as `exchange-core` (`Router::round_robin`).

`exchange-core` gains a `--print-digest-on-exit` flag that prints the same
digest lines at shutdown, computed from the live run's recorded outputs. The
e2e script can then compare live and replayed digests.

## Acceptance criteria

1. **Richer generator:** the determinism test's random command generator
   produces:
   - crossing limit orders (for trades);
   - market and IOC orders;
   - cancels and modifies that target ids of orders the generator has seen
     accepted (track them from completions);
   - occasional `BlockTrader`/`UnblockTrader`/`KillSwitch` broadcasts.

   A fixed seed reproduces the same command multiset except the target ids
   of cancels and modifies, which come from completions; the interleaving
   still varies.
2. **File journal round trip:** a live run with `FileJournalWriter` in a temp
   dir is replayed via `read_journal` → `replay`. Events, replies and book
   snapshots are identical to the live run for every shard.
3. **Digest stability:** replaying the same journal twice gives the same
   digest. Changing a single command in a copy of the journal changes it.
4. **Crash recovery:** truncate a shard journal mid-record, then
   `recover_tail`, then replay. The result equals the live output up to the
   last complete record.
5. **Restart:** `exchange-core` started again on a `--journal-dir` that
   already holds journals (for example after `docker compose stop` and
   `start`) starts successfully instead of failing on `O_EXCL`. Task 009 left
   this decision here: after `recover_tail`, either resume appending to the
   recovered file or start a new journal generation that replay reads in
   order. Record the choice in an ADR. A journal that `recover_tail` refuses
   (`Corrupt`, including a zero-filled tail that a crash under
   `--fsync=none` can leave) stops startup with a message naming the file;
   it is never deleted or truncated automatically.
6. **Tool check:** `scripts/e2e-smoke.sh` runs `lockstep-replay` on the
   run's journal dir and compares its digest lines with
   `--print-digest-on-exit` output.
7. The suite runs under tsan in under 30 s on CI hardware.
8. Presets debug, asan-ubsan and tsan pass; clang-tidy is clean.

## Files expected to change

- `exchange-core/tests/determinism/*` (generator, new test files) and its CMakeLists
- `exchange-core/app/include/lockstep/app/digest.hpp`, `exchange-core/app/src/digest.cpp`, `app/CMakeLists.txt`
- `exchange-core/main/src/replay_main.cpp` (new), `exchange-core/main/src/main.cpp` (flag), `exchange-core/main/CMakeLists.txt`
- `scripts/e2e-smoke.sh`
- `exchange-core/adapters/journal/*` (resume or new generation) and a new ADR

## Out of scope

- Snapshots/checkpoints to shorten replay (future ADR).
- Cross-shard global ordering (explicitly not a property, ADR-0003).

## Dependencies

- **Hard:** 002, 008, 009.
- **Soft:** 003, 004.
