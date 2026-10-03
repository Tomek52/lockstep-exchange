---
name: definition-of-done
description: Run and report Lockstep's full definition of done (CLAUDE.md section 4) for the current branch - all five CMake presets including TSan, architecture tests, format, clang-tidy, and Rust/proto/e2e when those areas changed - with the exact command and result for every item. Use before claiming a change is done, before marking a PR ready, or when asked for evidence.
---

# definition-of-done

Single source of truth: `.claude/skills/definition-of-done/SKILL.md` (shared with Claude Code; do not duplicate it here).
Read that file now and follow it exactly. Arguments for this invocation: $ARGUMENTS
(wherever the file says `$ARGUMENTS`, use them). Kiro differences: run agents with the
`subagent` tool using the role of the same name; subagent stages keep no context between
calls, so pass follow-up work with the spec path, diff range and findings verbatim.
