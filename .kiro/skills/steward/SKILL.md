---
name: steward
description: Repository conventions for driving a Lockstep pull request to green - how to map each CI job to a local command, fix failures, answer reviews, and keep history clean. Use when watching or babysitting a PR, handling a CI failure, or responding to review comments on a PR in this repository.
---

# steward

Single source of truth: `.claude/skills/steward/SKILL.md` (shared with Claude Code; do not duplicate it here).
Read that file now and follow it exactly. Arguments for this invocation: $ARGUMENTS
(wherever the file says `$ARGUMENTS`, use them). Kiro differences: run agents with the
`subagent` tool using the role of the same name; subagent stages keep no context between
calls, so pass follow-up work with the spec path, diff range and findings verbatim.
