# 16. Agent context budget

- **Status:** Proposed
- **Date:** 2026-09-30

## Context

Every Claude Code session and every subagent in the workflow
([ai-workflow.md](../ai-workflow.md)) starts with a fresh context. Whatever
enters it is paid for on every later turn of that agent: prompt caching makes
repeated input cheaper, but it still occupies the window and is re-read at a
fraction of the price each turn. Measured on this repository (2026-09-30):

- Claude Code injects `CLAUDE.md` into the main session and into every custom
  subagent before the first turn: probed without tools, the `verifier` agent
  quoted section 6 verbatim, while the built-in `Explore` agent reported
  that it had no `CLAUDE.md` in its context. Nevertheless the `developer`,
  `analyst`, `architect` and `concurrency-auditor` instructions told the
  agent to Read `CLAUDE.md`, which put a second copy (12.6 KB) into the
  context of every run.
- A green `ctest --preset debug` prints about 35 KB (279 lines, one per test
  and gtest's banner); the agents' fast loop ran it raw after every step, and
  the reviewers reran full builds to check claims.
- 2.0 KB of `CLAUDE.md` was the versioned-anchor rules of ADR-0015, which
  only matter when documentation is edited.
- The `analyst` read all of `ai-workflow.md` (10 KB) to use one section.

Quality must not pay for the savings: the rules an agent follows have to
reach it at the moment it needs them.

## Decision

1. **No duplicate loading.** Agent and skill instructions say that
   `CLAUDE.md` is already in context and must not be Read again. They still
   name the sections that bind the role.
2. **Quiet inner loop.** Agents build and test through
   `.claude/skills/definition-of-done/quick-check.sh`, which keeps the full
   output in `build/logs/` and prints one line when green, and only the
   compiler errors, failed assertions or sanitizer reports when red. The
   definition of done still runs through `run-dod.sh` with every preset.
   Logs are read with `grep` or `tail`, never whole.
3. **Path-scoped documentation rules.** The anchor rules move verbatim from
   `CLAUDE.md` to `.claude/rules/documentation.md`, with `paths:` covering
   `docs/**/*.md`, `README.md`, `ROADMAP.md` and `CLAUDE.md`. Claude Code loads
   the file when an agent reads a matching file: editing an existing file
   requires reading it, and the agents that create documents (`analyst`,
   `architect`) read the task or ADR index first. Probed in a subagent
   (`verifier`), the rule was absent before it read
   `docs/tasks/004-risk-controls-in-domain.md` and present right after.
   `CLAUDE.md` (now 11.3 KB)
   keeps a pointer and the two rules whose violation is
   hardest to undo (never change an anchor ID except by bumping it; never fix
   a broken versioned link by reverting or removing it). The `code-guard`
   and the `verifier` read the rule file explicitly when a diff touches
   Markdown, because working from a diff reads no documentation file.
4. **Narrow reads and reuse.** Agents read the section they use, not the
   whole document. Review findings go back to the `developer` agent that
   wrote the code, through `SendMessage`, instead of to a fresh one. Prompts
   to subagents pass paths and decisions, not file contents.
5. **Models unchanged.** The model split of the workflow stays: a developer
   and its reviewers on different models is a quality measure (reasons in
   [ai-workflow.md](../ai-workflow.md), "Agents and skills"), and the
   verifier's context is already small once logs stay in files.

## Alternatives considered

- **`@` imports from `CLAUDE.md`.** They load at launch, so they organise the
  file but save nothing.
- **A nested `docs/CLAUDE.md`.** Loads lazily too, but would not cover
  `README.md`, `ROADMAP.md` and `CLAUDE.md` at the root, and MkDocs would
  treat it as a page.
- **Moving the comment rules of section 2 to a path-scoped rule as well.**
  They apply to every code change and to every review, which often reads
  only a diff; the saving is not worth a rule that silently fails to load.
- **Running the verifier on a smaller model.** Its remaining work is
  judgement (is an ADR needed, are the docs up to date), and its context is
  small once logs stay in files; the saving is small and the risk is a
  wrong "pass".
- **Doing nothing.** The duplicate `CLAUDE.md` and the raw test output are
  paid for on every turn of every agent run, with no benefit.

## Consequences

- An agent that edits documentation without reading a matching file first
  (for example through a script) does not get the anchor rules
  automatically. The pointer in `CLAUDE.md` tells it to read the file; the
  docs check (`scripts/docs-check.sh`) still catches malformed anchors and
  broken versioned links, but not a missing version bump.
- `.claude/rules/` is not part of the MkDocs site, so its links are not
  checked by the strict build; `/docs-sync` greps it explicitly. Readers of
  the site see the anchor rules in ADR-0015, which holds them in full, and
  no longer on the agent-rules page.
- New agent or skill instructions follow the same rules: point to
  `CLAUDE.md` instead of asking for it to be read, use `quick-check.sh` for
  the inner loop, and keep raw logs out of the context.
