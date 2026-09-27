# 1. Record architecture decisions

- **Status:** Accepted
- **Date:** 2026-09-27

## Context

This repository is built by a human engineer working together with LLM coding
agents (see [docs/ai-workflow.md](../ai-workflow.md)). Agents start every task
without memory of earlier conversations. Whatever the reasons behind a
structural choice were, they will not be rediscovered by reading the code
alone, and an agent that does not know *why* a boundary exists is likely to
"simplify" it away.

A human reviewer has the same problem six months later.

## Decision

We record every architecturally significant decision as an ADR in `docs/adr/`,
using Michael Nygard's format (Context, Decision, Consequences), numbered
sequentially and never rewritten. A changed decision gets a new ADR that
supersedes the old one.

"Architecturally significant" means: it constrains how future code is
structured, it would be expensive to reverse, or a reasonable engineer could
have chosen differently. Examples: a dependency, a threading rule, a wire
format, a toolchain requirement.

The definition of done in [CLAUDE.md](../../CLAUDE.md) requires an ADR whenever
a task makes such a decision.

## Consequences

- Task specs and code comments can cite an ADR instead of repeating the
  argument (`// relaxed: see ADR-0011`).
- Reviewers can reject a change that contradicts an ADR without re-arguing it;
  the author must supersede the ADR first.
- Small overhead per decision. We accept it: the alternative is decisions that
  exist only in chat transcripts nobody will read again.
