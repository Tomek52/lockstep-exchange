---
description: Find documentation links broken by a versioned-anchor bump or removal, compare the old and new wording of each point, and update the referring docs (or list what needs a human decision).
argument-hint: "[git base ref, default: merge-base with origin/main]"
allowed-tools: Bash, Read, Grep, Glob, Edit
---

# Sync documentation references after a requirement change

Rules: CLAUDE.md, "Documentation anchors", and
[ADR-0017](../../docs/adr/0017-docs-site-and-versioned-anchors.md). The base
ref is `$ARGUMENTS`, or `git merge-base HEAD origin/main` if it is empty.

## 1. Run the check

```bash
scripts/docs-check.sh
```

The anchor lint runs first and prints one line per problem, as `file:line`:

- `stale link: T#X-vN is now #X-vM`: the point was changed and its version
  bumped;
- `broken link: T has no anchor "X-vN"`: the anchor was removed or renamed;
- anything else (format, marker, missing criterion anchor, duplicate): fix it
  in the defining document per CLAUDE.md, then re-run.

If the lint passes but the Docusaurus build fails, the build output names the
page route and the broken target. Map the route back to a file:
`/docs/tasks/003-x` → `docs/tasks/003-x.md`, `/ROADMAP` → `ROADMAP.md`,
`/` → `README.md`.

If everything passes, report that and stop.

## 2. For each stale or broken reference

1. **Old and new wording of the target point.** Find the commit that changed
   the anchor:

   ```bash
   git log -p --follow <base>..HEAD -- <target file>        # or:
   git log -S'<a id="X-vN">' --oneline -- <target file>
   ```

   Extract the complete point (the whole list item, paragraph or table) as it
   was at `-vN` and as it is at `-vM`. If the anchor disappeared, check
   whether the point was deleted, moved to another file
   (`git log -S'X-v' --all`), or renamed. Renaming breaks the rules, so report
   it.
2. **What the referring text relies on.** Read the sentence, table row or
   criterion around the link in the referring file, not just the link.
3. **Decide:**
   - **Still true under the new wording:** change the link to `#X-vM`. Change
     nothing else.
   - **Wording is now wrong, and the fix follows mechanically from the new
     point** (a renamed event, a changed value that is only restated): update
     the referring text and the link. If the referring point is itself
     anchored and its meaning changed, bump its `-vN` too, then loop back to
     step 1 for its own references.
   - **Needs judgement** (conflicting requirements, a changed ADR decision, a
     criterion whose intent may no longer hold, a deleted point): do not edit.
     Put it on the unresolved list.
4. Never "fix" a failure by pointing a link back to an old version, removing
   the link, or deleting an anchor.

## 3. Verify and report

Run `scripts/docs-check.sh` again until it passes, apart from the items on
your unresolved list.

Report:

| Reference (file:line) | Target | Old → new (one line each) | Action |
|---|---|---|---|

Action is one of: *link updated*, *text + link updated* (quote the change),
or *unresolved* (with why and the question a human must answer).

Do not commit. Leave the changes for review; any content change you made
must be visible in the diff.
