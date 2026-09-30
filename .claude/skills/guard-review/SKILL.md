---
name: guard-review
description: Review a Lockstep change (current diff, a branch, or a PR number) with the code-guard checklist - spec conformance, weakened tests, layer boundaries, hidden nondeterminism, concurrency, money, invented APIs, comment rules, unverified claims - and report verified findings by severity. Adds the concurrency-auditor for lock-free or threaded code. Use when asked to review, guard, or check a change before merge.
argument-hint: "[PR number | branch | empty for the current diff]"
---

# Guard review

## 1. Pick the target

- Empty `$ARGUMENTS`: `git diff origin/main...HEAD` plus uncommitted changes.
- A branch: `git diff origin/main...<branch>`.
- A PR number: read it with the GitHub tools (diff, description, linked
  task), and review the head commit.

Find the task spec it implements (`Task: NNN` in commit bodies or the PR
title) and read it with its ADRs.

## 2. Review

Run the `code-guard` agent on the target. Give it the diff range, the spec
path and the PR description if there is one; it reads the rest itself, so do
not paste the diff or the ADRs into the prompt. Also run the
`concurrency-auditor` agent in parallel when the diff touches any of:
`exchange-core/concurrency/`, `std::atomic`, `std::thread`/`jthread`,
`app/src/shard_runtime.cpp`, the publisher, or idle strategies.

## 3. Consolidate

Merge both reports, drop duplicates, keep the higher severity, and make sure
every finding has `path:line` and a concrete failure scenario. Anything
without evidence becomes a question, not a finding.

## 4. Deliver

- **Local review:** print the findings, blockers first, then the verdict.
- **PR review** (only when asked to post): one pending review with inline
  comments on the lines, submitted with `REQUEST_CHANGES` if there is any
  blocker or major, otherwise `COMMENT`. End every comment with the Claude
  Code attribution footer.
- If the review caught an LLM mistake worth remembering, propose an entry
  for **Lessons learned** in `docs/ai-workflow.md` (what was produced, why
  it was wrong, how it was caught, what prevents a repeat).
