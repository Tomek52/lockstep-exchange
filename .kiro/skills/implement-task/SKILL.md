---
name: implement-task
description: Drive one Lockstep task spec (docs/tasks/NNN-*.md) through the whole workflow - spec check, tests first, implementation, review by code-guard (and concurrency-auditor where relevant), full definition of done, commits, and a PR with evidence. Use when asked to implement, do, or pick up task NNN.
---

# implement-task

Single source of truth: `.claude/skills/implement-task/SKILL.md` (shared with Claude Code; do not duplicate it here).
Read that file now and follow it exactly. Arguments for this invocation: $ARGUMENTS
(wherever the file says `$ARGUMENTS`, use them). Kiro differences: run agents with the
`subagent` tool using the role of the same name; subagent stages keep no context between
calls, so pass follow-up work with the spec path, diff range and findings verbatim.
