# 15. Requirement tracing with OpenFastTrace

- **Status:** Proposed
- **Date:** 2026-09-29

## Context

Every task spec ends in acceptance criteria, and [ai-workflow.md](../ai-workflow.md)
makes "tests first" the core of the loop. Until now the link from a criterion
to the test that proves it, and to the code that implements it, existed only
in prose ("Tests in `risk_controls_test.cpp`", "Criterion 4" in a comment).
Nothing noticed when:

- a criterion had no test, or its test was deleted or renamed;
- a criterion was reworded after its tests were written, so the tests no
  longer proved what the spec says;
- an ADR decision that a criterion relies on had no implementation to point
  at.

Much of the code is written by LLM agents, which produce plausible claims of
coverage. A mechanical check makes those claims verifiable.

[OpenFastTrace](https://github.com/itsallcode/openfasttrace) (OFT) is a
requirement tracing tool: specification items with typed, versioned IDs live
in Markdown, coverage tags live in source comments, and `oft trace` reports
every missing, broken or outdated link. It is a single Java jar with no other
runtime dependencies, reads the Markdown we already write, and understands
C++, Rust and YAML comments.

## Decision

1. **Specs are OFT documents.** Milestones in `ROADMAP.md` are `feat` items;
   each acceptance criterion of a task spec is a `req` item covering its
   milestone; ADR decisions that a criterion relies on are `dsn` items in a
   `## Traceability` section at the end of the ADR, quoting the Decision
   without editing it. Code carries `impl` tags, tests `utest`/`itest` tags,
   and CI jobs `bld` tags for process criteria such as "presets pass".
   Syntax, ID format and revision rules: CLAUDE.md section 7.
2. **IDs are descriptive and stable**, never derived from a criterion's
   number. A semantic change to an item bumps its revision (`~1` → `~2`),
   which invalidates every link to it until the covering tests and code have
   been re-checked and their tags updated.
3. **A CI gate** (`scripts/oft-trace.sh`, job `trace`) fails on broken or
   outdated links in either direction, on `Depends` entries that name no
   existing item (OFT does not check those), and on missing coverage.
   Criteria of tasks not yet implemented carry `Status: proposed`; the gate
   allows them to be uncovered. The script has a self-test over fixtures,
   run in the same CI job.
4. **Pinned tool.** OpenFastTrace 4.10.0, downloaded by the script from the
   project's GitHub release and verified by SHA-256. It needs Java 17+.
5. **Pilot scope.** Milestone M1 (tasks 001–004) and the ADRs it relies on
   (0004, 0013) are converted. The remaining specs keep their current form
   until the pilot has been reviewed and this ADR accepted.

## Alternatives considered

- **Keep prose references.** No tooling, but nothing detects drift, which is
  the problem being solved.
- **A home-grown script** (grep for criterion numbers in test comments).
  Cheaper to start, but it would re-implement versioning, needed-coverage
  types and reporting that OFT already has, and number-based references
  break whenever a criterion is inserted.
- **Heavier requirement tools** (Doorstop, Sphinx-needs, commercial ALM).
  They need their own document formats or a Python documentation stack, and
  would move requirements out of the plain Markdown agents already read.
- **Filtering unfinished tasks with OFT's `--wanted-statuses`.** Tried first:
  an item that covers an excluded criterion (for example an ADR `dsn` item)
  becomes `orphaned`, so the filter cannot express "not implemented yet".
  The gate reads the full trace and exempts `proposed` items instead.

## Consequences

- A criterion without a test, a test tagged to a criterion that no longer
  exists, and a reworded criterion whose tests were not re-checked all fail
  CI. `build/oft/report.html` shows the whole chain, milestone to test.
- Tag comments in code and tests are a new kind of comment beside the "why"
  comments of CLAUDE.md section 2; section 7 allows them explicitly.
- A new tool dependency: Java in the `trace` CI job and for developers who
  run the gate locally (not needed for building or testing).
- Rewording a criterion now costs a revision bump and a re-check of its
  tests. That cost is intended; it is the point of the gate.
- OFT's Markdown parser has sharp edges (a one-line `Covers:` is silently
  ignored; an item runs until the next heading). CLAUDE.md section 7 lists
  them, and the gate catches their usual symptom: an item without coverage.
