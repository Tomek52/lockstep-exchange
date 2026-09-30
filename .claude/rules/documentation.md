---
paths:
  - "docs/**/*.md"
  - "README.md"
  - "ROADMAP.md"
  - "CLAUDE.md"
---

# Documentation rules

Moved out of CLAUDE.md so they load only when documentation is being read or
edited ([ADR-0016](../../docs/adr/0016-agent-context-budget.md)). Decision
behind them: [ADR-0015](../../docs/adr/0015-documentation-site-and-versioned-anchors.md).
They apply to Markdown under `docs/`, plus README, ROADMAP and CLAUDE.md.

- **Anchor IDs** have the [format](../../docs/adr/0015-documentation-site-and-versioned-anchors.md#adr0015-anchor-id-format-v1)
  `<doc>-<slug>-v<N>`: `<doc>` is `tNNN` (task spec), `adrNNNN` (ADR) or
  `arch` (`docs/architecture/`); the slug is 1–6 kebab-case words saying
  *what* is required, never a list position; `N` is the content version.
  Examples: `t004-blocked-trader-rejected-v1`, `adr0013-fail-closed-policy-v1`.
- An acceptance criterion starts with its anchor and a visible marker:
  `1. <a id="t004-blocked-trader-rejected-v1"></a>**[t004-blocked-trader-rejected v1]**`.
  Headings and other referenced items get only the anchor, first thing in
  the line: `## <a id="arch-matching-rules-v1"></a>Matching rules`. Use
  `<a id>`, not `{#id}` (GitHub shows `{#id}` literally).
- Link to the specific rule (`004-risk-controls-in-domain.md#t004-…-v1`), not
  to the whole file, whenever the context names one.
- **Never change an existing ID** except to bump its `-vN`; never reuse a
  retired ID for different content.
- **When you change the meaning of an anchored item**, follow the
  [version bump rule](../../docs/adr/0015-documentation-site-and-versioned-anchors.md#adr0015-version-bump-rule-v1): bump `-vN` in the
  anchor *and* the marker, run `scripts/docs-check.sh`, and review every
  reference it reports (and every other occurrence of the old ID: MkDocs
  reports each page only once). Move a link to the new version only after
  checking that the referring text is still true; if it is not, fix that
  text too (bumping its own version if it is anchored), or stop and ask.
  `/docs-sync` walks this process. Typo or formatting fixes that keep the
  meaning do not bump the version.
- Never "fix" a broken versioned link by reverting the target's version or
  by removing the link.
