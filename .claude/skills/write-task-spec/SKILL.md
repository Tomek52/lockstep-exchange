---
name: write-task-spec
description: Write a new self-contained task spec for Lockstep in docs/tasks/NNN-*.md (Goal, Context, Interfaces, Acceptance criteria, Files, Out of scope, Dependencies), and register it in the task table and ROADMAP. Use when someone describes new work, a bug or a feature that has no spec yet, or asks to split or tighten an existing spec.
argument-hint: "<short description of the work>"
---

# Write a task spec

Delegate to the `analyst` agent, or follow its instructions
(`.claude/agents/analyst.md`) directly. `$ARGUMENTS` describes the work.

## Steps

1. **Number.** Next free number after the highest in `docs/tasks/`.
2. **Research.** Read the code the task will touch, the ADRs for that area,
   and neighbouring specs. Use real names from the code.
3. **Draft** `docs/tasks/NNN-<kebab-name>.md` with these sections, in order:

   ```markdown
   # NNN: <Title>

   ## Goal
   ## Context
   ## Interfaces to implement
   ## Acceptance criteria
   ## Files expected to change
   ## Out of scope
   ## Dependencies
   ```

   - Acceptance criteria are numbered, observable and specific; each maps
     to a test.
   - Context links every binding ADR and names the current code.
   - Interfaces are real C++/Rust signatures, with `[[nodiscard]]` and
     `std::expected` where CLAUDE.md requires them.
4. **Decision needed?** If yes, the deliverables include an ADR; ask the
   `architect` agent to draft it (status Proposed).
5. **Register** the task: row in the table in `docs/tasks/README.md`
   (layer, hard deps, wave), the mermaid graph if dependencies changed, the
   ownership table if it owns new files, and an unticked checkbox in
   `ROADMAP.md`.
6. **Self-check** against "Writing a new task" in `docs/tasks/README.md`:
   could an agent with only this repository implement it, and a reviewer
   verify it from the acceptance criteria alone?

## Commit

`docs: add task NNN <short name>` with a body saying why the task exists.
