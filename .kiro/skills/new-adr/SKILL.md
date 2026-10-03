---
name: new-adr
description: Record an architecture decision for Lockstep as a new ADR in docs/adr/ (Nygard format, next number, status Proposed, index updated), or supersede an existing one without editing its decision. Use when a change introduces a new dependency, format, threading rule, protocol change, or any choice a reasonable engineer could have made differently.
---

# new-adr

Single source of truth: `.claude/skills/new-adr/SKILL.md` (shared with Claude Code; do not duplicate it here).
Read that file now and follow it exactly. Arguments for this invocation: $ARGUMENTS
(wherever the file says `$ARGUMENTS`, use them). Kiro differences: run agents with the
`subagent` tool using the role of the same name; subagent stages keep no context between
calls, so pass follow-up work with the spec path, diff range and findings verbatim.
