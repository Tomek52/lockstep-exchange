---
name: definition-of-done
description: Run and report Lockstep's full definition of done (CLAUDE.md section 4) for the current branch - all five CMake presets including TSan, architecture tests, format, clang-tidy, and Rust/proto/e2e when those areas changed - with the exact command and result for every item. Use before claiming a change is done, before marking a PR ready, or when asked for evidence.
---

# Definition of done

CLAUDE.md section 4 lists what "done" means. This skill runs every
automatable item and tells you what is left to check by reading.

## 1. Run the automated checks

```bash
.claude/skills/definition-of-done/run-dod.sh
```

It:
- marks untracked files under `exchange-core/`, `rust/`, `proto/` with
  `git add -N`, because `scripts/check-format.sh` only formats files that
  git tracks (a new file once reached CI unformatted this way);
- configures, builds and tests every preset: debug, release, clang-debug,
  asan-ubsan, **tsan** (never optional);
- runs `ctest -L architecture`, `scripts/check-format.sh`,
  `scripts/run-clang-tidy.sh`;
- runs the Rust and proto checks when `rust/` or `proto/` changed relative to
  `origin/main`, and tells you when the e2e smoke test applies;
- runs `scripts/docs-check.sh` when documentation changed.

Useful knobs: `PRESETS="debug tsan"` for a quick subset while iterating,
`BASE=<ref>` to diff against another base, `DOD_LOG_DIR=<dir>` to keep logs.
A subset is never enough for the final report. For a failing check, read its
log with `grep` or `tail`, never whole: a green `ctest` log alone is about
35 KB.

While iterating, `quick-check.sh` in this directory builds and tests one
preset (`PRESET=debug` by default; extra arguments go to `ctest`) and prints
one line when green, only the errors when red. It is the inner loop, not the
definition of done.

If a sanitizer preset aborts with "unexpected memory mapping", run
`sudo sysctl vm.mmap_rnd_bits=28` and rerun that preset.

## 2. Extra checks the script cannot do

- **Templates only instantiated in tests** are not seen by
  `run-clang-tidy.sh` (it covers library sources only). Run
  `clang-tidy-19 -p build/clang-debug <test-file>` on the test that
  instantiates them.
- **e2e:** when the script says wiring may have changed, run
  `cd rust && cargo build --locked && cd .. && scripts/e2e-smoke.sh`.

## 3. Check by reading

- Acceptance criteria: each has a test, and the tests were seen failing
  before the implementation (the failure is quoted in the PR).
- An ADR exists if a decision was made that a reasonable engineer could have
  made differently.
- ROADMAP checkbox, README status and architecture docs match the change.
- No new `TODO` without `TODO(task-NNN)`; no new `NOLINT` or clippy `allow`
  without a reason on the same line:
  `git diff origin/main...HEAD | grep -nE 'TODO|NOLINT|allow\('`.

## 4. Report

One row per CLAUDE.md section 4 item: the command, the result (pass counts
from the summary), or `not run` with the reason. Quote the first failing
lines of anything red. Never report a check you did not run as passing.
