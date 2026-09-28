---
name: implement-task
description: Drive one Lockstep task spec (docs/tasks/NNN-*.md) through the whole workflow - spec check, tests first, implementation, review by code-guard (and concurrency-auditor where relevant), full definition of done, commits, and a PR with evidence. Use when asked to implement, do, or pick up task NNN.
argument-hint: "<task number, e.g. 002>"
---

# Implement a task

This is the loop from `docs/ai-workflow.md`, with the project's agents in
their roles. `$ARGUMENTS` is the task number.

## 0. Preconditions

- Find the spec: `docs/tasks/$ARGUMENTS-*.md`. If none exists, stop and use
  the `write-task-spec` skill (analyst) first.
- Check its **Dependencies** against `ROADMAP.md`: every hard dependency must
  be ticked (merged). If not, stop and say which one blocks it.
- Work on the branch the session designates; otherwise create
  `task/NNN-<short-name>` from an up-to-date `origin/main`.

## 1. Read (no code yet)

`CLAUDE.md`, the spec, and **every ADR the spec links**. If the spec is
ambiguous, contradicts an ADR, or needs a decision the spec does not make,
stop and ask. A needed decision goes to the `architect` agent as an ADR,
not into the code.

## 2. Tests first (developer)

Write the acceptance tests the spec lists, in the files it names. Register
new test sources in the right `CMakeLists.txt` (one CTest label per
executable). Build and run; **copy the failure output**, it goes in the PR.

## 3. Implement (developer)

Smallest change that passes, within "Files expected to change". Iterate
with the fast loop:

```bash
cmake --build --preset debug && ctest --preset debug
```

Delegate to the `developer` agent if the work is large or you want a fresh
context; give it the spec path and the rules above.

## 4. Review

- Run the `code-guard` agent on the diff. Fix every blocker and major; fix
  plainly correct nits; answer the rest in the PR.
- If the change touches `concurrency/`, atomics, threads, the shard runtime
  or the publisher, also run the `concurrency-auditor` agent.

## 5. Definition of done

Use the `definition-of-done` skill. All five presets, format, clang-tidy,
and the conditional checks. Fix and rerun until clean.

## 6. Record

- Tick the task in `ROADMAP.md`; update README status and architecture docs
  if they changed.
- Commit in Conventional Commits (CLAUDE.md section 5), body `Task: NNN`,
  refactoring separate from behaviour changes, every commit green.

## 7. Pull request (only when asked, or when the session says to)

Title: `feat(<scope>): <summary> (task NNN)`. Body sections:
**What changed**, **Tests (written first)** with the quoted failure,
**Definition of done** as a checklist with commands and results,
**Notes for review** (decisions, risks, follow-ups). Then offer to watch
the PR for CI and review comments.
