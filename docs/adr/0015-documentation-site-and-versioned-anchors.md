# 15. Documentation site and versioned requirement anchors

- **Status:** Proposed
- **Date:** 2026-09-29

## Context

Task specs, ADRs and architecture notes refer to each other constantly:
a task depends on another task's primitive, an acceptance criterion restates
an ADR rule, the domain model names the task that owns a rule. Until now those
references were prose ("task 001", "ADR-0013") or links to whole files. Two
problems follow:

- A reader cannot jump to the exact rule being referenced.
- When a requirement changes, nothing tells the author which other documents
  relied on the old wording. With specs written and executed by LLM agents
  ([ai-workflow.md](../ai-workflow.md)), a stale cross-reference becomes a
  plausible-looking wrong implementation.

The sources must stay plain Markdown that renders on GitHub, and README.md,
ROADMAP.md and CLAUDE.md must stay at the repository root (tools and agents
read them there).

## Decision

**Site.** <a id="adr0015-site-toolchain-v1"></a>MkDocs 1.6 with the Material
theme renders `docs/`. Versions are pinned exactly in `requirements-docs.txt`.
MkDocs stays on 1.x: 2.0 removes the plugin system that Material and
include-markdown rely on.

- Root documents are rendered through one-line stub pages in `docs/`
  (`index.md`, `roadmap.md`, `agent-rules.md`) using
  `mkdocs-include-markdown-plugin`, which rewrites their relative links.
- Links from a document to anything outside `docs/` stay plain relative
  paths in the source. The hook `scripts/mkdocs_hooks.py` rewrites them at
  build time: root documents to their site pages, existing code paths to
  GitHub URLs. A path that does not exist is left alone so the build fails.
- Mermaid diagrams use Material's native `pymdownx.superfences` integration.
- The sources follow GitHub's Markdown dialect, not Python-Markdown's: the
  hook adds the blank line Python-Markdown needs before a list, and
  `mdx_truly_sane_lists` accepts the 2-space nested indent.
- On the site, the hook turns every versioned anchor into a self-link: the
  criterion marker becomes clickable, other anchors show their ID as a small
  label, and the item a link lands on is highlighted
  (`docs/stylesheets/anchors.css`). GitHub and editor previews show only the
  source form.

**Anchor IDs.** <a id="adr0015-anchor-id-format-v1"></a>A stable, versioned
anchor marks every acceptance criterion and every other section someone links
to. Format:

```
<doc>-<descriptive-slug>-v<N>
  <doc>  = tNNN (task spec) | adrNNNN (ADR) | arch (docs/architecture)
  slug   = 1–6 lowercase kebab-case words saying what is required
  N      = content version, starting at 1
```

The slug describes the content, never its position ("criterion 5"), so
renumbering a list does not break links. IDs are unique across the whole site.

- Acceptance criteria (list items) start with
  `<a id="t004-blocked-trader-rejected-v1"></a>**[t004-blocked-trader-rejected v1]**`.
  The visible marker repeats the ID with the version separated, so readers
  and reviewers see which version they are reading. The item's number stays.
  In a nested list, the leaves are the criteria.
- Headings, other paragraphs and list items (for example a bold
  pseudo-heading in an ADR) start with `<a id="..."></a>`
  (`## <a id="arch-matching-rules-v1"></a>Matching rules`) and carry no
  visible marker, so the document text is unchanged. The raw `<a id>` form is
  used everywhere, headings included, because it resolves both on the site
  and on GitHub.
- References to a specific rule link to its anchor
  (`004-risk-controls-in-domain.md#t004-blocked-trader-rejected-v1`), not to
  the file.

**Versioning rule.** <a id="adr0015-version-bump-rule-v1"></a>An existing ID
never changes except for its `-vN` suffix. When the *meaning* of an anchored
item changes, its suffix is bumped (in the anchor and the marker). Every link
to the old version then breaks the strict build, and each referring document
must be reviewed before its link is moved to the new version. Typo and
formatting fixes that keep the meaning do not bump the version. Deleting an
anchored item is a bump to nothing: its referrers must be updated too.

**Enforcement.** <a id="adr0015-enforcement-v1"></a>`scripts/docs-check.sh`
runs in CI and locally. It checks anchor IDs (format, global uniqueness,
marker matches anchor, every criterion anchored in adopted task specs), then
runs `mkdocs build --strict` with `validation.links.anchors`, `not_found`,
`unrecognized_links`, `absolute_links` and `validation.nav.omitted_files` set
to `warn`.

## Alternatives considered

- **Links to whole files only (status quo).** Nothing detects that a
  referenced rule changed.
- **IDs from list numbers (`t004-ac-5`).** Break or silently re-point
  whenever a criterion is inserted or removed.
- **IDs containing a hash of the item text.** Fully automatic change
  detection, but every typo fix breaks every referrer, and the IDs are
  unreadable. The manual `-vN` bump keeps the author in charge of what counts
  as a change of meaning.
- **`attr_list` heading IDs (`## Heading {#id}`).** Clean on the site, but
  GitHub renders `{#id}` literally in the heading and the link does not
  resolve there.
- **Sphinx + MyST.** More powerful cross-referencing (`{ref}` roles), but
  non-standard syntax that GitHub does not render, and a heavier toolchain.
- **Moving README/ROADMAP/CLAUDE.md into `docs/`.** No plugin needed, but
  breaks the paths that GitHub, Claude Code, the skills and the definition of
  done rely on.
- **Rewriting code links to absolute GitHub URLs in the sources.** No hook
  needed, but the links always point at `main` instead of the branch being
  browsed, and nothing checks that the target exists.

## Consequences

- References are clickable on the site and on GitHub (both resolve
  `<a id>` anchors).
- Changing the meaning of a referenced requirement is a two-step, reviewed
  operation: bump, then walk the build's failure list
  (`.claude/commands/docs-sync.md` automates the walk).
- The mechanism only sees Markdown links. Prose references ("criterion 5 of
  task 004"), references from code comments, and links from files outside
  `docs/` and the three root documents are not checked. New references
  should therefore be written as links.
- Forgetting to bump `-vN` after changing an item's meaning is not detected
  by the build; reviewers must check it (it is on the code-guard checklist
  via CLAUDE.md).
- Anchors are being introduced incrementally: tasks 001–004, the ADRs and
  architecture sections they reference form the pilot. Other documents keep
  file-level links until converted.
- Material and include-markdown are external Python dependencies, pinned and
  used only for the docs job; the C++ and Rust builds do not depend on them.
