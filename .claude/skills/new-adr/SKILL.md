---
name: new-adr
description: Record an architecture decision for Lockstep as a new ADR in docs/adr/ (Nygard format, next number, status Proposed, index updated), or supersede an existing one without editing its decision. Use when a change introduces a new dependency, format, threading rule, protocol change, or any choice a reasonable engineer could have made differently.
argument-hint: "<decision title>"
---

# New ADR

Delegate to the `architect` agent, or follow these steps. `$ARGUMENTS` is
the decision title.

1. **Number:** next after the highest in `docs/adr/` (check the index table
   in `docs/adr/README.md` too).
2. **File:** `docs/adr/NNNN-<kebab-title>.md`:

   ```markdown
   # NN. <Title>

   - **Status:** Proposed
   - **Date:** YYYY-MM-DD
   - **Supersedes:** ADR-XXXX   <!-- only if it does -->

   ## Context
   The forces at play, with links to code, tasks and other ADRs.

   ## Decision
   What we do, stated so it can be checked in review.

   ## Alternatives considered
   Each option and why it lost.

   ## Consequences
   What gets easier, what gets harder, what we now must enforce.
   ```

3. **Status:** stays `Proposed`. Only the human marks it `Accepted`.
4. **Superseding:** in the old ADR change only the status line to
   `Superseded by ADR-NNNN`. Never edit an accepted decision.
5. **Index:** add the row to the table in `docs/adr/README.md`.
6. **Links:** reference the ADR from the task spec or code comment
   (`// see ADR-NNNN`) that motivated it.
7. **Enforcement:** if the decision can be checked by the build (a
   `static_assert`, an include rule, a CTest), say so in Consequences and
   add it in the implementing task.

Commit: `docs(adr): propose ADR-NNNN <title>`.
