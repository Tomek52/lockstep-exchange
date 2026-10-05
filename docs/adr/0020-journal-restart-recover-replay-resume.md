# 20. Journal restart: recover, replay, resume appending

- **Status:** Accepted
- **Date:** 2026-10-05

## Context

Task 009 made `FileJournalWriter::create()` refuse (`O_EXCL`) to open a
journal file that already exists, and left the restart decision to task 010:
what happens when `exchange-core` starts with `--journal-dir` pointing at a
directory that already holds `shard-<id>.jnl` files, for example after
`docker compose stop` and `start`, or a crash and restart?

Two things must both be true after a restart:

- the exchange must **resume**, not restart from empty state: in-flight order
  ids, book contents and sequence numbers must continue exactly where the
  previous run left off, so a trader's open orders are still there;
- the restart must not corrupt or discard a journal that `recover_tail`
  (task 009) cannot safely repair.

## <a id="adr0020-decision-v1"></a>Decision

On startup, for each shard journal that already exists in `--journal-dir`:

1. Run `recover_tail` on it. This cuts a torn tail (a crash mid-write) to the
   last complete, CRC-valid record; it never touches a file it refuses.
2. If `recover_tail` refuses the file (`JournalError::Corrupt` - including a
   zero-filled tail, which a crash under `--fsync=none` can leave, since
   zero bytes decode as a record header of length zero with a CRC that
   almost never matches - or `VersionMismatch`/`IoFailure`), **startup fails**
   with a message naming the file. The file is never deleted or truncated
   automatically; recovering it is an operator decision.
3. Re-read the recovered file with `ReaderExpectations` set to this run's
   shard id, shard count and `config_hash` (ADR-0017). A mismatch
   (`JournalError::ConfigMismatch`) also fails startup naming the file: the
   journal was written under a different shard layout or `ShardConfig`, and
   replaying it under this one would silently diverge (ADR-0004).
4. Replay the recovered records into that shard's fresh `ShardEngine`
   **before the runtime threads start**, so order ids, books and the
   sequence counter continue where they stopped. These replayed outputs are
   **not** published to market-data subscribers, gRPC clients or the risk
   link: nothing new happened, this is state rebuild, not new activity. The
   events and replies already reached their original recipients during the
   run that produced them.
5. Reopen the same file for appending (`O_APPEND`, no `O_EXCL`; new journals
   still use `O_EXCL` + mode `0600` to create) and continue the sequence
   numbering from the last recovered record's sequence number.

A journal directory with no existing file for a shard is unaffected: that
shard starts exactly as it does today (`FileJournalWriter::create`).

**Digest on exit.** `--print-digest-on-exit` must report the digest of the
*whole* journal - the state rebuilt from any pre-existing records plus this
run's own - not just what this run appended, so that it always equals
`lockstep-replay` run afterwards over the same directory. The simplest way to
guarantee that equality is to compute it the same way `lockstep-replay` does:
re-read each shard's committed file from disk at shutdown and replay it into
a fresh `ShardEngine`, rather than accumulate a running digest from the live
run's in-memory outputs (which would have to separately account for the
replayed-at-startup outputs that step 4 deliberately does not publish, and
could drift from the on-disk bytes if the two code paths diverged).

## Alternatives considered

- **A new journal generation per start** (`shard-0.jnl.2`, `.3`, ...): avoids
  ever reopening a file for writing, but replay must then chain generations
  in order, the directory accumulates more files to manage, and the header
  still needs its own bookkeeping to know which generation is current.
  Rejected: it does not remove the need to rebuild state (step 4 is still
  required), so it adds bookkeeping for no benefit here.
- **Start with empty state**, journaling the restart like any other run:
  simplest to implement, but a replay of the file from disk would then
  diverge from the live run (order ids and books would restart from zero
  while the file's later records still reference the old ones). Rejected: it
  breaks the determinism property (ADR-0004) the journal exists to provide.

## Consequences

- Startup time now grows with journal length: every restart replays the
  whole file before accepting traffic. Bounding this needs snapshots/
  checkpoints, which is out of scope here (a future ADR, also noted in
  task 010).
- `FileJournalWriter` gains an append-mode open path alongside `create()`;
  both still enforce owner-only permissions and never truncate existing
  content.
- The restart path and `lockstep-replay` share the same recover/replay
  building blocks (`recover_tail`, `read_journal`, `app::replay`), so a bug
  in one is likely to be caught by the other's tests.
- An operator who wants to discard a refused journal (rather than repair or
  archive it) must do so explicitly, outside the exchange; nothing in this
  decision automates that.
