---
name: write-task-spec
description: Write a new self-contained task spec for Lockstep in docs/tasks/NNN-*.md (Goal, Context, Interfaces, Acceptance criteria, Files, Out of scope, Dependencies), and register it in the task table and ROADMAP. Use when someone describes new work, a bug or a feature that has no spec yet, or asks to split or tighten an existing spec.
---

# write-task-spec

Single source of truth: `.claude/skills/write-task-spec/SKILL.md` (shared with Claude Code; do not duplicate it here).
Read that file now and follow it exactly. Arguments for this invocation: $ARGUMENTS
(wherever the file says `$ARGUMENTS`, use them). Kiro differences: run agents with the
`subagent` tool using the role of the same name; subagent stages keep no context between
calls, so pass follow-up work with the spec path, diff range and findings verbatim.
