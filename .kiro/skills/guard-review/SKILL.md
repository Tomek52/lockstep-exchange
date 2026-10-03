---
name: guard-review
description: Review a Lockstep change (current diff, a branch, or a PR number) with the code-guard checklist - spec conformance, weakened tests, layer boundaries, hidden nondeterminism, concurrency, money, invented APIs, comment rules, unverified claims - and report verified findings by severity. Adds the concurrency-auditor for lock-free or threaded code. Use when asked to review, guard, or check a change before merge.
---

# guard-review

Single source of truth: `.claude/skills/guard-review/SKILL.md` (shared with Claude Code; do not duplicate it here).
Read that file now and follow it exactly. Arguments for this invocation: $ARGUMENTS
(wherever the file says `$ARGUMENTS`, use them). Kiro differences: run agents with the
`subagent` tool using the role of the same name; subagent stages keep no context between
calls, so pass follow-up work with the spec path, diff range and findings verbatim.
