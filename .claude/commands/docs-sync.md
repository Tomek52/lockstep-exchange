---
description: Repair documentation references broken by a versioned-anchor bump (ADR-0015) - run the docs check, compare old and new wording of each changed item, update referrers that are still true, and list what needs a human.
argument-hint: "[base ref, default: merge-base with origin/main]"
---

# Docs sync

Goal: after someone bumped the `-vN` of an anchored item (a task criterion,
an ADR section, an architecture rule), bring every reference to it up to date
**without** letting a reference silently keep claiming something the new
wording no longer says. Rules: CLAUDE.md, "Documentation"; decision:
`docs/adr/0015-documentation-site-and-versioned-anchors.md`.

Base for "old wording": `$ARGUMENTS` if given, else
`git merge-base HEAD origin/main`.

## 1. Collect the broken references

Run `scripts/docs-check.sh`. If it passes, go to step 5.

- A failure in the "anchor conventions" part (format, duplicate, marker
  mismatch, missing criterion anchor) is not a sync problem: fix it at its
  source first, then rerun.
- From the MkDocs part, take every warning of the form
  `Doc file 'A' contains a link 'B#ID-vN', but the doc 'B' does not contain an anchor '#ID-vN'`
  (or `... no such anchor on this page` for a link within one file).
  Paths are relative to `docs/`; `index.md`, `roadmap.md` and
  `agent-rules.md` stand for `README.md`, `ROADMAP.md` and `CLAUDE.md`.
- MkDocs reports each (page, anchor) pair **once**. For every broken ID, also
  run `git grep -n "#<ID>" -- docs README.md ROADMAP.md CLAUDE.md` to find
  all occurrences.
- Any other warning (missing file, page not in nav) is outside this command:
  report it and do not guess.

## 2. Pair each broken ID with its new version

For a broken `<base>-vN` in target file `B`:

- `git grep -n 'id="<base>-v' -- <B>` finds the current version `M`.
- If `M > N`: the item was bumped; continue with step 3.
- If there is no `<base>-v*` in `B`: the item was removed, renamed or moved.
  Look for it with `git log -S 'id="<base>-vN"' --oneline -- docs` and report
  it as **needs a human**; do not re-point the link to a guess.
- Never lower `M` or restore the old anchor to make the build pass.

## 3. Compare the old and new wording

- Old: the item as it was at the base,
  `git show <base-ref>:<B>` (the list item, paragraph or section starting at
  `id="<base>-vN"`, up to the next item of the same level).
- New: the same span in the working tree at `id="<base>-vM"`.
- Show the two side by side (a `git diff --no-index --word-diff` of the two
  extracted spans is the clearest).
- If several versions were skipped (N+1 < M), compare N with M directly and
  also read the intermediate commits (`git log -p -S 'id="<base>-v' -- <B>`).

## 4. Decide for every referring occurrence

Read the referring sentence (and its list item or paragraph) in context,
then classify:

| Verdict | When | Action |
|---|---|---|
| **still true** | The referring text makes no claim the change affects (for example it only says "comes from task 001", or cites a part that did not change). | Change the link to `#<base>-vM`. Nothing else. |
| **mechanical update** | The referring text restates a value, name or order from the item, the new value is unambiguous, and the referring item is not an accepted ADR's decision. | Update the restated text **and** the link. If the edited referring item has its own versioned anchor, its meaning changed too: bump it and add it to the list for another round. |
| **needs a human** | Anything else: the change alters scope, behaviour, acceptance or an assumption the referrer builds on; the referrer is an accepted ADR decision (supersede, never edit); the correct new wording is a judgement call. | Leave the link on `-vN` so the build stays red, and report it. |

When unsure, choose **needs a human**. A red build with a clear report is
the correct outcome; a green build with a wrong reference is not.

## 5. Check and report

- Rerun `scripts/docs-check.sh` after each round until the only failures
  left are the **needs a human** ones.
- Also run `git diff <base-ref> -- docs README.md ROADMAP.md CLAUDE.md`
  and look for anchored items whose text changed while their `-vN` did not.
  List them as "possible missing bump": the build cannot see these.
- Report a table, one row per occurrence:

  | Referring file:line | Link | Old → new | Verdict | Why / what changed |
  |---|---|---|---|---|

  followed by the exact `scripts/docs-check.sh` result, and for each
  **needs a human** row the old and new wording and the question to decide.
- Do not commit unless asked. If asked, use one commit per changed target
  item: `docs: sync references to <base>-vM`, with the body listing the
  updated referrers and the verdicts.
