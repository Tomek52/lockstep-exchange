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
   zero-filled tail, which a crash under `--fsync=none` can leave: the
   record header's `payload_size`/`crc32c` fields read as zero, and
   CRC32C of an empty payload is itself zero, so the header's own check
   passes; `decode_payload` is what refuses it, since zero bytes can never
   decode to a real command - or `VersionMismatch`/`IoFailure`), **startup
   fails** with a message naming the file. The file is never deleted or
   truncated automatically; recovering it is an operator decision.
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
   still use `O_EXCL` + mode `0600` to create). `open_for_append` does not
   re-check the file's permission bits: verifying them needs `fstat`
   (`<sys/stat.h>`), which the journal adapter's architecture rule
   (ADR-0002) does not let it include (that header has a `/` in its name,
   which this layer's fitness function treats the same as any other
   third-party header). A file this reopens keeps whatever mode it already
   has, including one changed outside the exchange after `create()` made it
   owner-only; `open_for_append`'s doc comment says so. Continue the
   sequence numbering from the last recovered record's sequence number.
6. Before accepting any traffic from outside (gRPC order entry, the risk
   link), broadcast `RiskLinkStatus{connected=false}` to every shard as this
   run's own first live command, journaled like any other. A crash can leave
   the recovered state's last known link status as `connected=true` - the
   link really was up when the process died - and resuming that naively
   would let `RiskLinkPolicy::FailClosed` treat the link as live with
   nothing actually connected on this run. This is unconditional (a fresh
   start journals it too, harmlessly: `RiskState::set_link_connected` is
   idempotent, and a fresh `ShardEngine` already starts disconnected), so a
   fresh start's journal has the same shape as a resumed one, and the
   decision does not depend on inspecting the resumed state first.

A journal directory with no existing file for a shard is unaffected: that
shard starts exactly as it does today (`FileJournalWriter::create`).

**Digest on exit.** `--print-digest-on-exit` must report the digest of the
*whole* journal - the state rebuilt from any pre-existing records plus this
run's own - not just what this run appended, so that it always equals a
`lockstep-replay` run afterwards over the same directory. The first version
of this decision computed it by re-reading each shard's committed file from
disk at shutdown, the same way `lockstep-replay` does. Review (task 010)
rejected that: a mutant that passed a no-op resume factory (silently
skipping step 4 entirely) still passed `scripts/e2e-smoke.sh`, because
re-reading from disk makes the check a replay-equals-replay tautology - it
cannot catch a bug in how, or whether, the live run itself produced that
state in the first place.

The fix is `app::DigestBuilder`: each shard's `ShardRuntime` can own one and
folds into it incrementally - every record resumed in step 4 (not
published, but still folded, in file order), then every command the live
run processes, interleaved per command (that command's events, then its
reply) in the exact order a from-disk replay would produce them. The owner
thread writes it via `resume_from_journal()`, before any shard thread
exists; from then on only the shard thread writes it, via `process()`; a
std::jthread's construction happens-after everything its starting thread
did beforehand, so the owner thread's writes are visible to the shard
thread without further synchronisation, and `--print-digest-on-exit` reads
it only after `Engine::stop()` has joined the shard thread, for the same
reason in reverse. `app::digest(ReplayOutput, books)` - what
`lockstep-replay` uses - is implemented on top of the same `DigestBuilder`,
so the two can never fold their output differently by accident; aligning
them this way also surfaced and fixed a second bug, where `digest()` had
folded all events before all replies instead of interleaving per command.
Memory stays bounded (one running hash and two counters per shard, not the
growing output vectors a from-disk replay builds).

**Folding is opt-in** (`EngineConfig::record_digest`, `ShardRuntime::
Config::record_digest`; `ShardRuntime::digest_builder()` returns
`std::optional<DigestBuilder>`, `nullopt` when disabled). Measured on this
machine at roughly 180ns per resting `NewOrder` when enabled - about 45% of
`apply()` itself - which is not something every run should pay for a
feature most runs never ask for. `main.cpp` enables it only when
`--print-digest-on-exit` is passed; tests that need the digest enable it
explicitly. Resuming into a shard with digest recording enabled still folds
every resumed record (the `if (digest_)` guard is inside
`resume_from_journal()`, not around the call to it), so a restart with
`--print-digest-on-exit` still reports the whole journal's digest, not just
what this run appended.

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
- `FileJournalWriter` gains an append-mode open path alongside `create()`.
  `create()` still enforces owner-only permissions and never truncates
  existing content; `open_for_append()` never truncates either, but does
  not re-verify permissions (see step 5) - an operator who wants that
  checked has to do it themselves, outside the exchange.
- The restart path and `lockstep-replay` share the same recover/replay
  building blocks (`recover_tail`, `read_journal`, `app::replay`), so a bug
  in one is likely to be caught by the other's tests.
- An operator who wants to discard a refused journal (rather than repair or
  archive it) must do so explicitly, outside the exchange; nothing in this
  decision automates that.
