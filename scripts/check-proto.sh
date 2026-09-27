#!/usr/bin/env bash
# Proto consistency gate (ADR-0006). Run locally before committing proto changes;
# CI runs the same script.
#
#   scripts/check-proto.sh                 # lint + format + breaking vs. main
#   scripts/check-proto.sh --against REF   # breaking check vs. another git ref
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
against="main"
if [[ "${1:-}" == "--against" ]]; then against="$2"; fi

cd "${repo_root}/proto"

echo "==> buf lint"
buf lint

echo "==> buf format"
buf format --diff --exit-code

echo "==> buf breaking (against ${against})"
if git -C "${repo_root}" rev-parse --verify --quiet "${against}" >/dev/null \
   && git -C "${repo_root}" cat-file -e "${against}:proto/buf.yaml" 2>/dev/null; then
  buf breaking --against "${repo_root}/.git#ref=${against},subdir=proto"
else
  echo "    skipped: ref '${against}' has no proto/ baseline yet"
fi

echo "proto checks passed"
