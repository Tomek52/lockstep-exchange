# 17. Docusaurus docs viewer and versioned requirement anchors

- **Status:** Proposed
- **Date:** 2026-09-29

## Context

Task specs, ADRs and architecture docs reference each other in prose
("the matching events as in task 002", "see ADR-0013"). Two problems follow:

- A reference names a document, not the point it relies on, so a reader has
  to hunt for it.
- When a requirement changes, nothing tells the author which other documents
  relied on its old wording. With LLM agents executing specs literally, a
  stale reference is a silent spec bug.

The Markdown files are read mostly by developers, directly on GitHub or in an
editor, so they must stay plain Markdown. A rendered site is for wide reviews
and for readers who are not developers (product owner, analyst).

ADR-0015 and ADR-0016 are reserved by tasks 012 and 015, hence 0017.

## Decision

**Viewer.**

- Docusaurus 3 (`website/`, pinned by `website/package-lock.json`) renders
  `docs/**/*.md`, `README.md`, `ROADMAP.md` and `CLAUDE.md` in place. It
  uses one docs plugin instance rooted at the repository with that `include`
  list. Nothing is moved or copied.
- `.md` files are parsed as CommonMark (`markdown.format: 'detect'`), never
  MDX. No front matter, imports or JSX in the docs: the sources must read the
  same on GitHub.
- `onBrokenLinks`, `onBrokenAnchors` and
  `markdown.hooks.onBrokenMarkdownLinks` are `throw`.
- Relative links to repository files that are not docs pages (source code,
  `LICENSE`, directories) stay relative in the sources. A remark plugin
  (`website/src/remark-repo-links.js`) turns them into GitHub URLs at build
  time.

**Versioned anchors.**

- An anchor is raw HTML, `<a id="ID"></a>`, placed at the start of the list
  item, paragraph or heading it marks. It is not a `{#id}` heading ID:
  GitHub shows `{#id}` literally and cannot link to it.
- ID format: `<prefix>-<descriptive-words>-v<N>`.
  - Prefixes: `tNNN` for task specs, `adrNNNN` for ADRs, `arch` for
    `docs/architecture/`.
  - The words describe the requirement, never its list number.
  - `vN` is the version of the content.
- Every acceptance criterion of a task that uses anchors gets one. Other
  points get one only when another document references them.
- Task anchors are followed by a visible marker, `**[<prefix>-<words> vN]**`.
  ADR and architecture anchors have none.
- Changing the content of an anchored point bumps `vN`. Every link to the old
  version then fails the build, and its author must re-read the reference
  before retargeting it.
- `scripts/docs-check.sh` (in CI) lints anchors and builds the site.

## Alternatives considered

- **`{#id}` custom heading IDs:** GitHub renders them as text, and links to
  them do not work in GitHub's preview. They also require the point to be a
  heading.
- **Small headings per criterion:** pollute the table of contents. They
  still need `<a id>` for GitHub, because GitHub derives heading slugs from
  the text.
- **An MDX anchor component:** makes the files MDX, which GitHub does not
  render.
- **`<span id>`:** not registered by Docusaurus's broken-anchor check
  (verified). Only `<a>` goes through its Link component.
- **Docs plugin on `../docs` plus a second instance for the root files:** the
  instances overlap, and markdown links do not resolve across instances. The
  build fails (verified).
- **Copying or moving README/ROADMAP into `docs/`:** copying adds generated
  files and link rewriting. Moving changes GitHub's landing page and the
  paths that CLAUDE.md relies on.
- **Rewriting code links to GitHub URLs in the sources:** they would point
  at `main`, not at the branch being read.
- **Numbered IDs (`t004-ac2`):** they break when a criterion is inserted, and
  they say nothing about what the reference relies on.

## Consequences

- A content change to an anchored requirement cannot merge until every
  reference to it has been revisited (CI job `docs`). References from code
  comments or from outside the site are not checked.
- `website/` adds a Node toolchain (Node ≥ 20) for documentation only. The
  C++ and Rust builds do not depend on it.
- Docusaurus 3.10's Mermaid theme needs its optional
  `@mermaid-js/layout-elk` peer; without it the client bundle does not
  compile.
- Bare `<Type>` outside backticks is parsed as an HTML tag and disappears,
  on GitHub too. CLAUDE.md lists this pitfall.
- Adopting anchors in the remaining tasks is incremental. The lint enforces
  complete acceptance-criteria coverage only in task files that already use
  anchors.
