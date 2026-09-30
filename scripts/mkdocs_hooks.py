"""MkDocs hook: resolve links that leave docs/ (ADR-0015).

The Markdown sources stay browsable on GitHub, so they link to code and to the
root documents with plain relative paths. On the site those targets either do
not exist (code) or live under another name (README.md -> index.md). This hook
rewrites such links before MkDocs validates them:

- README.md, ROADMAP.md, CLAUDE.md at the repository root -> their site pages,
  keeping the #anchor so that anchor validation still applies;
- any other existing file or directory outside docs/ -> its GitHub URL;
- a target that does not exist is left untouched, so `validation.links`
  reports it and `mkdocs build --strict` fails.

It also papers over two places where Python-Markdown, which MkDocs uses,
differs from GitHub's CommonMark and the sources follow GitHub:

- a list needs a blank line before it (otherwise it is glued to the previous
  paragraph): `_separate_lists` inserts one outside code fences;
- nested blocks need a 4-space indent: `mdx_truly_sane_lists` in mkdocs.yml
  accepts the 2-space indent used here.

It also makes versioned anchors visible on the site. In the sources an anchor
is an empty `<a id="...">`, which gives a reader who followed a link nothing to
see. Each one becomes a self-link: a criterion's `**[id vN]**` marker is
wrapped in it, and any other anchor shows its ID as a small label
(docs/stylesheets/anchors.css), so the target of a link can be identified and
its URL copied.
"""

from __future__ import annotations

import posixpath
import re
from pathlib import Path

# Root documents that are rendered as site pages via include-markdown stubs.
ROOT_PAGES = {
    "README.md": "index.md",
    "ROADMAP.md": "roadmap.md",
    "CLAUDE.md": "agent-rules.md",
}

# Inline Markdown links and images: ](target) or ](<target>), optional title.
_LINK = re.compile(r"(\]\()(<[^>]+>|[^)\s]+)((?:\s+\"[^\"]*\")?\))")
_FENCE = re.compile(r"^\s*(```|~~~)")
_SCHEME = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")
_LIST_ITEM = re.compile(r"^\s*(?:\d+\.|[-*+])\s+\S")
_CODE_SPAN = re.compile(r"(`+).*?\1")
_ANCHOR = re.compile(r'<a id="([a-z0-9-]+)-v(\d+)"></a>(\*\*\[[^\]]*\]\*\*)?')


def _separate_lists(lines: list[str]) -> list[str]:
    """Adds the blank line Python-Markdown needs before a list (GitHub does not)."""
    out: list[str] = []
    in_fence = in_list = False
    for line in lines:
        if _FENCE.match(line):
            in_fence = not in_fence
        elif not in_fence:
            if not line.strip():
                in_list = False
            elif _LIST_ITEM.match(line):
                if not in_list and out and out[-1].strip():
                    out.append("\n")
                in_list = True
        out.append(line)
    return out


def _decorate_anchor(m: re.Match[str]) -> str:
    anchor_id = f"{m.group(1)}-v{m.group(2)}"
    label = f"{m.group(1)} v{m.group(2)}"
    if m.group(3):  # Criterion: the visible marker becomes the self-link.
        return (f'<a id="{anchor_id}" class="ls-anchor ls-criterion" href="#{anchor_id}" '
                f'title="Link to this criterion">{m.group(3)}</a>')
    return (f'<a id="{anchor_id}" class="ls-anchor" href="#{anchor_id}" '
            f'data-label="{label}" aria-label="Anchor {label}" title="Link to this section"></a>')


def _decorate_anchors(line: str) -> str:
    """Decorates anchors outside `inline code` (examples of the syntax stay literal)."""
    out, pos = [], 0
    for code in _CODE_SPAN.finditer(line):
        out.append(_ANCHOR.sub(_decorate_anchor, line[pos:code.start()]))
        out.append(code.group(0))
        pos = code.end()
    out.append(_ANCHOR.sub(_decorate_anchor, line[pos:]))
    return "".join(out)


def _rewrite(target: str, page_dir: Path, docs_dir: Path, repo_root: Path,
             page_uri_dir: str, blob_base: str) -> str:
    bare = target[1:-1] if target.startswith("<") else target
    if not bare or bare.startswith(("#", "/")) or _SCHEME.match(bare):
        return target
    path, sep, anchor = bare.partition("#")
    resolved = (page_dir / path).resolve()
    if resolved == docs_dir or docs_dir in resolved.parents:
        return target  # A normal docs link: MkDocs validates it itself.
    try:
        rel = resolved.relative_to(repo_root).as_posix()
    except ValueError:
        return target  # Outside the repository: let validation report it.
    if rel in ROOT_PAGES:
        site_path = posixpath.relpath(ROOT_PAGES[rel], page_uri_dir or ".")
        return f"{site_path}{sep}{anchor}"
    if resolved.is_dir():
        return f"{blob_base.replace('/blob/', '/tree/', 1)}{rel}"
    if resolved.is_file():
        return f"{blob_base}{rel}{sep}{anchor}"
    return target


def on_page_markdown(markdown: str, page, config, files) -> str:  # noqa: ANN001 (MkDocs hook API)
    docs_dir = Path(config["docs_dir"]).resolve()
    repo_root = Path(config["config_file_path"]).resolve().parent
    page_dir = Path(page.file.abs_src_path).resolve().parent
    page_uri_dir = posixpath.dirname(page.file.src_uri)
    ref = config["extra"].get("source_ref", "main")
    blob_base = f"{config['repo_url'].rstrip('/')}/blob/{ref}/"

    out: list[str] = []
    in_fence = False
    for line in _separate_lists(markdown.splitlines(keepends=True)):
        if _FENCE.match(line):
            in_fence = not in_fence
        if not in_fence:
            line = _LINK.sub(
                lambda m: m.group(1)
                + _rewrite(m.group(2), page_dir, docs_dir, repo_root, page_uri_dir, blob_base)
                + m.group(3),
                line,
            )
            line = _decorate_anchors(line)
        out.append(line)
    return "".join(out)
