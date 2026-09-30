#!/usr/bin/env bash
# Documentation gate (ADR-0015): anchor conventions, then a strict MkDocs build
# in which every broken link, missing anchor or page left out of the nav fails.
#
# Needs the pinned toolchain from requirements-docs.txt, either on PATH or in
# .venv-docs/ (python3 -m venv .venv-docs && .venv-docs/bin/pip install -r requirements-docs.txt).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [[ -x .venv-docs/bin/mkdocs ]]; then
  mkdocs=(.venv-docs/bin/mkdocs)
elif command -v mkdocs >/dev/null; then
  mkdocs=(mkdocs)
else
  echo "docs-check: mkdocs not found. Install it with:" >&2
  echo "  python3 -m venv .venv-docs && .venv-docs/bin/pip install -r requirements-docs.txt" >&2
  exit 2
fi

echo "==> anchor conventions"
python3 scripts/check-doc-anchors.py

echo "==> mkdocs build --strict"
site_dir="$(mktemp -d)"
trap 'rm -rf "${site_dir}"' EXIT
# We pin mkdocs < 2 on purpose; Material's banner about 2.0 is noise here.
NO_MKDOCS_2_WARNING=1 "${mkdocs[@]}" build --strict --site-dir "${site_dir}"
