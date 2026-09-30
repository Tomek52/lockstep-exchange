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

Documentation (Markdown under `docs/`, plus README, ROADMAP and this file),
[ADR-0015](docs/adr/0015-documentation-site-and-versioned-anchors.md):
- **Anchor IDs** have the [format](docs/adr/0015-documentation-site-and-versioned-anchors.md#adr0015-anchor-id-format-v1)
  `<doc>-<slug>-v<N>`: `<doc>` is `tNNN` (task spec), `adrNNNN` (ADR) or
  `arch` (`docs/architecture/`); the slug is 1–6 kebab-case words saying
  *what* is required, never a list position; `N` is the content version.
  Examples: `t004-blocked-trader-rejected-v1`, `adr0013-fail-closed-policy-v1`.
- An acceptance criterion starts with its anchor and a visible marker:
  `1. <a id="t004-blocked-trader-rejected-v1"></a>**[t004-blocked-trader-rejected v1]**`.
  Headings and other referenced items get only the anchor, first thing in
  the line: `## <a id="arch-matching-rules-v1"></a>Matching rules`. Use
  `<a id>`, not `{#id}` (GitHub shows `{#id}` literally).
- Link to the specific rule (`004-risk-controls-in-domain.md#t004-…-v1`), not
  to the whole file, whenever the context names one.
- **Never change an existing ID** except to bump its `-vN`; never reuse a
  retired ID for different content.
- **When you change the meaning of an anchored item**, follow the
  [version bump rule](docs/adr/0015-documentation-site-and-versioned-anchors.md#adr0015-version-bump-rule-v1): bump `-vN` in the
  anchor *and* the marker, run `scripts/docs-check.sh`, and review every
  reference it reports (and every other occurrence of the old ID: MkDocs
  reports each page only once). Move a link to the new version only after
  checking that the referring text is still true; if it is not, fix that
  text too (bumping its own version if it is anchored), or stop and ask.
  `/docs-sync` walks this process. Typo or formatting fixes that keep the
  meaning do not bump the version.
- Never "fix" a broken versioned link by reverting the target's version or
  by removing the link.

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
scripts/docs-check.sh                    # doc anchors + mkdocs build --strict
                                         # (first: python3 -m venv .venv-docs &&
                                         #  .venv-docs/bin/pip install -r requirements-docs.txt)

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
- [ ] If documentation changed: `scripts/docs-check.sh` passes, and every
      anchored item whose meaning changed has its `-vN` bumped.
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
