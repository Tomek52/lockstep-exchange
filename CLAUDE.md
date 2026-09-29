# CLAUDE.md: rules for AI agents working in this repository

You are working on **Lockstep**: a deterministic exchange engine (C++23,
`exchange-core/`) with a Rust risk service and load generator (`rust/`),
talking over gRPC (`proto/`). Architecture overview:
[docs/architecture/README.md](docs/architecture/README.md). Decisions and their
reasons: [docs/adr/](docs/adr/README.md). The workflow you are part of:
[docs/ai-workflow.md](docs/ai-workflow.md). Its roles are available as
subagents in `.claude/agents/` and its steps as skills in `.claude/skills/`
(see "Agents and skills" in ai-workflow.md).

If a task spec (`docs/tasks/NNN-*.md`) was given to you, it is your contract.
Read it, and every ADR it links, **before** writing code.

## 1. Boundaries you must not cross

These rules are enforced by the build and by tests. Do not work around the
enforcement: do not add NOLINTs, change allow-lists or edit fitness functions
to make a violation pass. If a rule blocks a task, stop and say so.

**Domain** (`exchange-core/domain`), [ADR-0002](docs/adr/0002-hexagonal-architecture-enforced-by-the-build.md):
- Standard library only, and only the headers allowed in
  `tests/architecture/CMakeLists.txt`.
- No threads, atomics, mutexes, clocks, I/O, randomness, gRPC or protobuf.
- No exceptions thrown. Failures are `std::expected<…, RejectReason>`
  ([ADR-0008](docs/adr/0008-error-handling-strategy.md)).
- No locks, ever. The single-writer principle
  ([ADR-0003](docs/adr/0003-single-writer-sharding.md)) makes them unnecessary.

**Determinism** ([ADR-0004](docs/adr/0004-deterministic-replay-via-per-shard-journal.md)):
- Every input that affects domain output must be part of a journaled
  `Command`. Time comes from `SequencedCommand::timestamp`, never from a
  clock.
- Output order must never depend on iterating a hash container. Iterate
  `flat_map`s or sort.
- Rejected commands are journaled too. Do not "optimise" them away.

**App layer** (`exchange-core/app`): no gRPC, no protobuf, no files. Talk to
the outside only through the ports in `app/include/lockstep/app/ports/`.

**Adapters** depend on `app`; never the reverse. A namespace inside
`lockstep::` must not be called `grpc` (it would shadow `::grpc`); use
`grpc_adapter`.

**Concurrency** ([ADR-0011](docs/adr/0011-lock-free-queues-and-memory-ordering.md)):
- Use the weakest correct memory ordering.
- **Every** non-`seq_cst` atomic operation gets a comment saying what it
  pairs with, or why no ordering is needed.
- Separate independently-written fields with
  `alignas(concurrency::cache_line_size)`.
- Only the owning shard thread may touch a `ShardEngine`.

**Money** ([ADR-0005](docs/adr/0005-fixed-point-prices-and-quantities.md)):
prices are integer ticks and quantities integer lots, as strong types.
Never use floating point for prices, quantities or PnL.

**Contracts** ([ADR-0006](docs/adr/0006-grpc-service-and-stream-design.md)):
- Changes in `proto/lockstep/v1` must be backward compatible:
  `scripts/check-proto.sh` runs `buf breaking`.
- Never renumber or reuse fields or enum values.
- Both builds compile the same `.proto` files; never copy them.

**Journal format** ([ADR-0012](docs/adr/0012-journal-binary-format.md)):
- Store `CommandTag` values, never `variant::index()`.
- Any format change bumps `format_version`.

**Decisions:** never edit an ADR's decision. Write a new ADR that supersedes
it, and update the index.

## 2. Coding conventions

C++ (formatting by `.clang-format`, naming enforced by `.clang-tidy`):
- `snake_case` functions and variables, `CamelCase` types and concepts,
  trailing `_` on private members, namespaces `lockstep::<layer>`.
- Headers under `<layer>/include/lockstep/<layer>/`, included as
  `"lockstep/<layer>/x.hpp"`.
- `[[nodiscard]]` on everything returning `std::expected`, `std::optional`
  or `bool` status.
- Exhaustive `switch` over enums, ending in `std::unreachable()` (no
  `default:`), so `-Wswitch` catches new enumerators.
- Use C++23 where it fits (see the table in
  [ADR-0009](docs/adr/0009-toolchain-baseline-and-feature-fallbacks.md)).
  Don't use a feature just to show it off.
- Bind `flat_map` elements with `auto&&`/`const auto&`, never `auto&`
  (`std::flat_map` iterators return proxies).
- Comments explain *why*, not what. Cite ADRs (`// relaxed: see ADR-0011`).
  Full rules: "Comments and in-code documentation" below.
- `constexpr` + `static_assert` for logic that can be checked at compile time.

Comments and in-code documentation (C++ and Rust):
1. **Readable code first.** Descriptive names, small functions, named
   constants instead of magic numbers. Express values with units (prices,
   quantities, timestamps, durations) as strong types, never a bare integer
   or `double`. Express possible failures in the signature
   (`std::expected`, `Result`), not in a comment.
2. **Comments only for *why*.** Non-obvious requirements (exchange rules,
   determinism), workarounds, warnings (call order, required thread,
   pointer lifetime), references to ADRs and tasks. Never a comment that
   restates the code. When you work out a non-obvious reason behind existing
   behaviour, add a comment and flag it in the change summary for
   verification.
3. **Doxygen (`///`) only for public interfaces** of the domain, the app
   layer and its ports. Describe the contract: preconditions, units, edge
   cases, errors, threading, lifetime of returned pointers. Not for private
   functions, trivial getters, test code or adapters' internals.
4. **No commented-out code, and no `TODO` without a task reference**
   (`// TODO(task-NNN): ...`) in new or changed code. Do not remove
   existing ones as part of an unrelated change; propose that separately.
5. **Comments must stay true.** When code changes, update or delete comments
   it made false. When moving code, move its *why* comments with it. Do not
   delete an existing comment just because you do not understand it; list
   doubtful ones in the change summary.

Rust (`rust/`): `cargo fmt`; clippy pedantic with `-D warnings` (workspace
lints). No `unsafe` (forbidden). No `unwrap()` outside tests and startup.
Keep pure logic (`positions`, `limits`, `engine`) free of async and I/O.

Tests:
- Exactly **one CTest label per test executable** (`unit`, `determinism`,
  `architecture`, `grpc`, `fuzz`).
- Anything linking gRPC or protobuf must be labelled `grpc`, which keeps it
  out of the TSan run ([ADR-0007](docs/adr/0007-dependency-management-system-packages.md)).

## 3. How to build and test

Everything runs on Ubuntu 24.04 (native, WSL2, or the CI image). First time:
`scripts/setup-ubuntu.sh`.

```bash
# C++: configure + build + test (presets: debug, release, clang-debug, asan-ubsan, tsan)
cmake --workflow --preset debug          # or step by step:
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan

ctest --preset debug -L architecture     # layer rules only
ctest --preset debug -R Matching         # one suite

# Rust
cd rust && cargo fmt --all -- --check && cargo clippy --all-targets --locked -- -D warnings && cargo test --locked

# Quality gates
scripts/check-format.sh                  # clang-format + rustfmt + buf format (--fix to apply)
scripts/run-clang-tidy.sh                # clang-tidy over all owned C++ sources
scripts/check-proto.sh                   # buf lint + breaking check
scripts/oft-trace.sh                     # requirement tracing gate (Java 17+), see section 7

# Cross-language smoke test (build debug preset and `cargo build` first)
scripts/e2e-smoke.sh

# Whole stack
docker compose -f deploy/docker-compose.yml up --build
```

If sanitizers abort with "unexpected memory mapping", run
`sudo sysctl vm.mmap_rnd_bits=28` (the setup script persists it).

## 4. Definition of done

A change is done only when **all** of these hold. Report each item
explicitly; don't claim a check you did not run.

- [ ] The task spec's acceptance criteria pass, with tests written first
      (see ai-workflow.md).
- [ ] `cmake --build` + `ctest` pass on **all presets**: debug, release,
      clang-debug, asan-ubsan and tsan. TSan is never optional; the runtime
      is multi-threaded by design.
- [ ] `scripts/run-clang-tidy.sh` and `scripts/check-format.sh` are clean.
- [ ] If Rust changed: fmt, clippy (`-D warnings`) and tests pass.
- [ ] If `proto/` changed: `scripts/check-proto.sh` passes, and both builds
      compile.
- [ ] If the wiring changed: `scripts/e2e-smoke.sh` passes.
- [ ] `scripts/oft-trace.sh` passes. In a converted spec, every acceptance
      criterion you implemented is covered by tags on its tests and code
      (section 7).
- [ ] **An ADR was added** if you made a decision a reasonable engineer could
      have made differently (a new dependency, format, threading rule,
      protocol change).
- [ ] Docs touched by the change are updated: architecture diagrams, README
      status, task spec checkboxes in ROADMAP.md.
- [ ] No new `TODO` without a task reference (`TODO(task-NNN)`). No new
      suppression (`NOLINT`, clippy `allow`) without a reason in the same
      line or block.

## 5. Commits

[Conventional Commits](https://www.conventionalcommits.org/): `type(scope): summary`.

- **Types:** `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`,
  `ci`, `chore`.
- **Scopes:** `domain`, `concurrency`, `app`, `journal`, `codec`, `grpc`,
  `risk-client`, `main`, `bench`, `fuzz`, `proto`, `rust`, `sentinel`,
  `loadgen`, `cmake`, `docker`, `adr`.
- Imperative mood, ≤ 72 characters in the summary. The body explains *why*
  and references the task (`Task: 005`) and any ADR.
- Small, logical commits. **Every commit must build and pass its tests.**
  Don't mix refactoring with behaviour changes.
- End AI-assisted commits with a `Co-Authored-By:` trailer naming the model.

## 6. Known pitfalls in this repo

- `gtest_discover_tests` (CMake 3.28) flattens list-valued labels. Use one
  label per executable (`lockstep_add_test` enforces it).
- System gRPC is not TSan-instrumented. TSan reports inside `epoll_wait` or
  `memmove` from a gRPC test mean the test is missing its `grpc` label, not
  that there is a real race.
- `try_push` must not consume its argument on failure. Retry loops depend on
  it.
- `std::flat_map` does not exist in libstdc++ 14. Use
  `lockstep::domain::flat_map`.
- `std::stacktrace` needs `lockstep::stacktrace` (it links `-lstdc++exp`).
- When unsure whether a C++23 feature is available, don't guess: add a probe
  to `cmake/FeatureProbe.cmake`.

## 7. Requirement tracing (OpenFastTrace)

Specs, code and tests are linked with [OpenFastTrace](https://github.com/itsallcode/openfasttrace)
(OFT), decided in [ADR-0015](docs/adr/0015-requirement-tracing-with-openfasttrace.md).
Converted so far: milestone M1 (tasks 001–004) and ADRs 0004 and 0013. The
other specs keep their numbered criteria until they are converted; do not
convert them as a side effect of another change.

**Artifact types.**

| Type | What | Where | `Needs` |
|---|---|---|---|
| `feat` | a milestone | `ROADMAP.md` | `req` |
| `req` | one acceptance criterion (one sub-bullet = one item) | `docs/tasks/NNN-*.md` | see below |
| `dsn` | an ADR decision that a criterion relies on | `## Traceability` at the end of the ADR | `impl` |
| `impl` | the code that implements a `req` or `dsn` | tag in C++/Rust | – |
| `utest` | a test from a `unit`-labelled executable, or a Rust unit test | tag above the test | – |
| `itest` | a `determinism`, `grpc` or e2e test | tag above the test | – |
| `bld` | a CI job that is the evidence for a process criterion | tag in `.github/workflows/ci.yml` | – |

A behaviour criterion `Needs: impl, utest` (`itest` where its test is an
integration test). If an ADR decision is the solution, it `Needs: dsn, utest`
instead and the `dsn` item needs `impl`. A process criterion ("presets
pass", "clang-tidy is clean") `Needs: bld`.

**IDs.** `<type>~<scope>.<descriptive-name>~<revision>`, lowercase kebab-case:

<!-- oft:off -->
```text
req~risk-controls.duplicate-block-trader-only-acks~1
dsn~risk-loop.idempotent-risk-commands~1
feat~matching-core~1
```
<!-- oft:on -->

- `scope` is the task's slug (`order-book-storage`, `matching`, `modify`,
  `risk-controls`) or the ADR's slug (`deterministic-replay`, `risk-loop`).
  A new spec picks its slug once, from its title, never from its number.
- The name says what the item requires. Never derive it from the
  criterion's number or position: numbers are for reading order only and
  stay in the heading (`### AC 6: ...`).
- **Never change an ID's type, scope or name.** Tests, code and other specs
  refer to it. If the meaning becomes something else entirely, remove the
  old item with all its links and add a new one.
- **When the content of an item changes meaning, bump its revision**
  (`~1` → `~2`) and, in the same commit, update every `Covers`, `Depends`
  and tag that refers to it, after re-checking that each test and piece of
  code still does what the new text says. The gate reports every link you
  missed as `outdated`. A typo or rewording with the same meaning does not
  bump the revision, and neither does a `Status` change.

**Markdown syntax** (OFT's parser is strict; these are verified, not
guessed):

<!-- oft:off -->
```markdown
### AC 6: A duplicate BlockTrader changes nothing but still acks
`req~risk-controls.duplicate-block-trader-only-acks~1`

Duplicate `BlockTrader` with the same id: second application changes
nothing, but still acks.

Covers:
- [feat~matching-core~1](../../ROADMAP.md#m1-matching-core)

Depends:
- [req~order-book-storage.cancel-if-by-trader~1](001-order-book-storage.md#ac-2e-cancel_if-by-trader-cancels-in-the-documented-order)

Needs: dsn, utest
```
<!-- oft:on -->

- Every item gets its own heading, with the ID alone on the next line in
  backticks. An item runs until the next heading, so put no prose after
  `Needs:`.
- `Covers:` and `Depends:` take a bulleted list only. `Covers: some-id` on
  one line is silently treated as description text.
- A list entry may be a Markdown link, as above, so the reference is
  clickable on GitHub. The anchor is GitHub's slug of the target heading;
  if a heading changes, the link breaks but the trace does not.
- `Depends:` records hard dependencies between criteria (a spec's
  "Dependencies" section). OFT does not check it; the gate does.
- In an ADR, never put items inside the Decision. Add or extend a
  `## Traceability` section at the end, quoting the Decision (which stays
  authoritative).
- `Status: proposed` goes on the line right after the ID (see below).

**Tags in code, tests and CI.**

<!-- oft:off -->
```cpp
// [utest->req~risk-controls.duplicate-block-trader-only-acks~1]
TEST_F(RiskControlsTest, DuplicateBlockTraderStillAcksButChangesNothing) {
```
<!-- oft:on -->

- Tests: directly above `TEST`/`TEST_F` (or `#[test]`). Tag every test that
  proves the criterion, not just one.
- Code: `impl` tags on the narrowest definition that implements the item
  (a function, or the branch inside it), not on a whole file.
- One tag per line, under 100 columns: clang-format reflows longer comment
  lines, which breaks the tag.
- Tags are the one kind of comment that is not a *why* comment (section 2).
  Keep existing comments next to them.
- Never tag a test to a criterion it does not actually check, just to
  make the trace pass.

**Tasks not implemented yet.** Their criteria (and `dsn` items that nothing
implements yet) carry `Status: proposed`, and the gate allows them to be
uncovered. When the task is done, delete those lines in the same change that
ticks its box in `ROADMAP.md`; the gate prints a note for any `proposed` item
that is already fully covered.

**The gate.** `scripts/oft-trace.sh` traces `docs/`, `ROADMAP.md`,
`exchange-core/`, `rust/crates/` and `.github/workflows/`, writes
`build/oft/report.html`, and fails on broken or outdated links, on broken
`Depends`, and on missing coverage of any item that is not `proposed`.
`--self-test` checks the gate itself against `scripts/oft-fixtures/`. Do not
work around a failure: no `<!-- oft:off -->` around real items, no tags on
tests that do not check the item, no edits to the gate or its fixtures to
make a violation pass (section 1 applies).
