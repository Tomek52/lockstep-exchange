---
name: steward
description: Repository conventions for driving a Lockstep pull request to green - how to map each CI job to a local command, fix failures, answer reviews, and keep history clean. Use when watching or babysitting a PR, handling a CI failure, or responding to review comments on a PR in this repository.
---

# PR steward conventions for Lockstep

## CI jobs and how to reproduce them locally

| CI job | Local command |
|---|---|
| `C++ <preset>` (debug, release, clang-debug, asan-ubsan, tsan) | `cmake --preset P && cmake --build --preset P && ctest --preset P` |
| `Format + clang-tidy` | `git add -N <new files>`, then `scripts/check-format.sh` and `scripts/run-clang-tidy.sh` |
| `Fuzz (60 s)` | `cmake --preset asan-ubsan && cmake --build --preset asan-ubsan --target order_entry_decode_fuzzer`, then `mkdir -p corpus && build/asan-ubsan/exchange-core/fuzz/order_entry_decode_fuzzer corpus -max_total_time=60` |
| `Rust` | `cd rust && cargo fmt --all -- --check && cargo clippy --all-targets --locked -- -D warnings && cargo test --locked` |
| `Proto consistency` | `scripts/check-proto.sh` |
| `End-to-end smoke` | debug build, `cd rust && cargo build --locked`, `scripts/e2e-smoke.sh` |

Always reproduce the failure locally first, then show the same command
passing before pushing.

## Known failure causes

- **Format job red, local check green:** the new file was untracked when you
  ran `check-format.sh` (it uses `git ls-files`). `git add -N` and rerun;
  fix with `scripts/check-format.sh --fix`.
- **clang-format version drift:** the scripts call `clang-format-19`; a
  plain `clang-format` may be 18 and format differently.
- **TSan reports inside gRPC** (`epoll_wait`, `memmove`): the test is
  missing its `grpc` label; fix the label, not the test.
- **Sanitizer "unexpected memory mapping":** `sudo sysctl vm.mmap_rnd_bits=28`,
  an environment issue, not a code bug.

A failing test is never an infrastructure flake until the same test passes
on an identical rerun of the same commit. Never skip, disable or weaken a
test to get green.

## History

- On branches you created (`claude/*`, `task/*`), add new commits; do not
  rewrite pushed history unless the user asks. On someone else's branch,
  never rebase, amend or force-push; merge `origin/main` instead.
- Commits follow CLAUDE.md section 5: Conventional Commits, `Task: NNN` in
  the body, every commit green, refactors separate from behaviour changes.

## Reviews

- Small, local asks (renames, nits, an added test, a one-function
  refactor): implement, push, reply briefly, resolve the thread.
- Asks that change an interface, a format, or an ADR decision: reply with a
  proposal and ask the author; a decision change needs a new ADR (the
  `new-adr` skill), not an edit.
- After fixing, rerun the `definition-of-done` skill before pushing and
  update the PR description's checklist with the new results.
