# Lockstep project rules

The rules for every agent in this repository live in `CLAUDE.md` (boundaries, conventions,
definition of done, commit style). It is the single source of truth, shared with Claude Code.
If it is not already in your context, read it before changing anything.

When you read or edit `docs/**/*.md`, `README.md`, `ROADMAP.md` or `CLAUDE.md`, also read
`.claude/rules/documentation.md` (versioned anchors, ADR-0015).

Project workflows (`implement-task`, `write-task-spec`, `new-adr`, `guard-review`,
`definition-of-done`, `docs-sync`, `steward`) are skills; roles (analyst, architect, developer,
code-guard, concurrency-auditor, verifier) are agents in `.kiro/agents/`.
