# 4. Deterministic replay via a per-shard write-ahead journal

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

Being able to rebuild the exact state of an exchange from its inputs gives us:

- crash recovery;
- audit ("why did this fill happen?");
- post-mortems, by replaying production input on a laptop;
- a very strong test oracle: the live system and a single-threaded replay of
  its journal must agree exactly.

Replay only works if *everything* that influences the outcome is captured as
input. Hidden inputs are the classic failure: wall-clock reads, random
numbers, iteration order of hash maps, thread interleavings, configuration
read at runtime, and messages from other systems that change state (here,
risk commands).

## Decision

<a id="adr0004-determinism-property-v1"></a>**The property.** For each shard, the sequence of `(CommandResult, events)`
produced by `ShardEngine::apply` is a pure function of:

- the shard's `ShardConfig`, and
- the sequence of `SequencedCommand`s in its journal.

Rules that make this true:

1. **Write-ahead.** The shard runtime appends each command to the journal
   *before* applying it, and releases the batch's outputs only after
   `Journal::commit()`. An output that clients saw is therefore always
   recoverable.
2. **Time is an input.** The runtime stamps each command with
   `Clock::now()`, and the timestamp is journaled inside the
   `SequencedCommand`. The domain has no access to a clock (enforced by
   ADR-0002's include rules).
3. **Rejections are inputs too.** A command the domain rejects is still
   journaled: it consumed a sequence number, and replay must reproduce the
   rejection.
4. **External state changes are commands.** Risk commands (`BlockTrader`,
   `UnblockTrader`, `KillSwitch`) and connectivity changes (`RiskLinkStatus`)
   enter the same ingress queues and are journaled (ADR-0013).
5. <a id="adr0004-no-hidden-nondeterminism-v1"></a>**No hidden nondeterminism in the domain.**
   - Output order never depends on hash-container iteration: iterate
     `flat_map`s (ordered), or sort first.
   - No randomness.
   - Order ids come from a per-shard counter.
6. **Config is pinned.** The journal header records shard id, shard count and
   a hash of the shard's instrument specs; replay refuses a mismatch
   (ADR-0012).

**Testing.** `tests/determinism/` runs:

- a live multi-threaded engine with several racing producers;
- then, for each shard, a replay of that shard's journal into a fresh
  `ShardEngine` via `app::replay()`.

It requires identical events and identical replies for every sequence
number. Task 010 extends it to the file journal, matching and book-snapshot
comparison.

## Consequences

- Recovery = read journal → `replay()` → resume at the next sequence number.
  No separate snapshotting is needed until journals grow large (a future ADR).
- Every feature must answer "what are its inputs?" before it is designed.
  This is the most frequent reason a design is sent back in review.
- Journal I/O sits on the shard thread's critical path. Batching (one
  `commit()` per drained batch) amortises it; the fsync policy is a
  configuration choice of the journal adapter (task 008).
- Determinism is per shard, not global (ADR-0003).
