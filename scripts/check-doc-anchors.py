#!/usr/bin/env python3
"""Checks versioned documentation anchors (ADR-0015).

MkDocs validates that every linked anchor exists. It does not check the
anchors themselves, which is what this script does:

1. every `<a id="...">` outside code matches `<doc>-<slug>-v<N>`, and <doc> matches the
   file it is in (tNNN in docs/tasks/NNN-*.md, adrNNNN in docs/adr/NNNN-*.md,
   arch in docs/architecture/);
2. every ID is unique across the documentation;
3. a visible marker `**[<id without -vN> vN]**` right after an anchor repeats
   the same ID and version;
4. in a task spec that has adopted anchors, every acceptance criterion (each
   leaf item of the list under "## Acceptance criteria") starts with an
   anchor and its marker.

Exit status 1 with one line per problem, in `file:line: message` form.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"
ROOT_DOCS = [REPO / "README.md", REPO / "ROADMAP.md", REPO / "CLAUDE.md"]

ID_FORMAT = re.compile(r"^(t\d{3}|adr\d{4}|arch)-[a-z0-9]+(?:-[a-z0-9]+){0,5}-v[1-9]\d*$")
ANCHOR = re.compile(r'<a id="([^"]*)"></a>(\*\*\[([^\]]*)\]\*\*)?')
LIST_ITEM = re.compile(r"^(\s*)(?:\d+\.|[-*+])\s+(.*)$")
FENCE = re.compile(r"^\s*(```|~~~)")
# Examples of the syntax in `inline code` are not anchors.
CODE_SPAN = re.compile(r"(`+).*?\1")
HEADING = re.compile(r"^(#{1,6})\s")
CRITERIA_HEADING = re.compile(r"^##\s+(?:<a id=\"[^\"]*\"></a>)?Acceptance criteria\s*$")


def expected_prefix(path: Path) -> str | None:
    rel = path.relative_to(REPO).as_posix()
    if m := re.match(r"docs/tasks/(\d{3})-", rel):
        return f"t{m.group(1)}"
    if m := re.match(r"docs/adr/(\d{4})-", rel):
        return f"adr{m.group(1)}"
    if rel.startswith("docs/architecture/"):
        return "arch"
    return None


def unfenced_lines(path: Path) -> list[tuple[int, str]]:
    lines, in_fence = [], False
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
            lines.append((number, CODE_SPAN.sub("", line)))
    return lines


def criteria_leaves(lines: list[tuple[int, str]]) -> list[tuple[int, str]]:
    """Leaf list items of the list under '## Acceptance criteria'."""
    items: list[tuple[int, int, str]] = []  # (line number, indent, text)
    inside = False
    for number, line in lines:
        if CRITERIA_HEADING.match(line):
            inside = True
            continue
        if inside and HEADING.match(line):
            break
        if inside and (m := LIST_ITEM.match(line)):
            items.append((number, len(m.group(1)), m.group(2)))
    leaves = []
    for i, (number, indent, text) in enumerate(items):
        has_child = i + 1 < len(items) and items[i + 1][1] > indent
        if not has_child:
            leaves.append((number, text))
    return leaves


def main() -> int:
    files = sorted(DOCS.rglob("*.md")) + [p for p in ROOT_DOCS if p.exists()]
    problems: list[str] = []
    seen: dict[str, str] = {}

    for path in files:
        rel = path.relative_to(REPO).as_posix()
        prefix = expected_prefix(path)
        lines = unfenced_lines(path)
        has_marker = False

        for number, line in lines:
            for m in ANCHOR.finditer(line):
                anchor_id, marker = m.group(1), m.group(3)
                where = f"{rel}:{number}"
                if not ID_FORMAT.match(anchor_id):
                    problems.append(f"{where}: anchor '{anchor_id}' does not match "
                                    "<tNNN|adrNNNN|arch>-<1-6 word slug>-v<N> (ADR-0015)")
                    continue
                if anchor_id.split("-", 1)[0] != prefix:
                    problems.append(f"{where}: anchor '{anchor_id}' must start with "
                                    f"'{prefix}-' in this file" if prefix else
                                    f"{where}: versioned anchors are not used in this file")
                if anchor_id in seen:
                    problems.append(f"{where}: anchor '{anchor_id}' already defined at {seen[anchor_id]}")
                else:
                    seen[anchor_id] = where
                if marker is not None:
                    has_marker = True
                    base, version = anchor_id.rsplit("-v", 1)
                    if marker != f"{base} v{version}":
                        problems.append(f"{where}: marker '[{marker}]' does not match anchor "
                                        f"'{anchor_id}' (expected '[{base} v{version}]')")

        if prefix and prefix.startswith("t") and has_marker:
            for number, text in criteria_leaves(lines):
                m = ANCHOR.match(text)
                if not m or m.group(3) is None:
                    problems.append(f"{rel}:{number}: acceptance criterion has no "
                                    "anchor and marker (ADR-0015)")

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f"check-doc-anchors: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"check-doc-anchors: {len(seen)} anchors OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
