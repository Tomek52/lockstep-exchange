#!/usr/bin/env bash
# Documentation gate (ADR-0017): lints versioned anchors, then builds the
# Docusaurus site, which fails on any broken link, anchor or markdown link.
# CI runs the same script.
#
#   scripts/docs-check.sh           # installs website/node_modules if missing
#   scripts/docs-check.sh --ci      # always `npm ci` (clean, lockfile-exact)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
website="${repo_root}/website"

echo "==> versioned anchors"
node "${website}/scripts/check-anchors.mjs"

if [[ "${1:-}" == "--ci" || ! -d "${website}/node_modules" ]]; then
  echo "==> npm ci"
  npm ci --prefix "${website}" --no-audit --no-fund
fi

echo "==> docusaurus build"
log="$(mktemp)"
trap 'rm -f "${log}"' EXIT
if ! npm run --prefix "${website}" build 2>&1 | tee "${log}"; then
  echo "docs build failed: see the broken links/anchors listed above" >&2
  exit 1
fi
# Warnings (e.g. a deprecated option) must not accumulate silently.
if grep -q '\[WARNING\]' "${log}"; then
  echo "docs build emitted warnings; fix them" >&2
  exit 1
fi

echo "docs checks passed"
