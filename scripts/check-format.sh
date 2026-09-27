#!/usr/bin/env bash
# Formatting gate for all languages. `--fix` rewrites files instead of checking.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
fix=0
[[ "${1:-}" == "--fix" ]] && fix=1

mapfile -t cpp_files < <(git ls-files -- 'exchange-core/*.cpp' 'exchange-core/*.hpp' \
  | grep -v '/fixtures/')

echo "==> clang-format (${#cpp_files[@]} files)"
if [[ ${fix} -eq 1 ]]; then
  clang-format-19 -i "${cpp_files[@]}"
else
  clang-format-19 --dry-run --Werror "${cpp_files[@]}"
fi

echo "==> cargo fmt"
if [[ ${fix} -eq 1 ]]; then
  (cd rust && cargo fmt --all)
else
  (cd rust && cargo fmt --all -- --check)
fi

echo "==> buf format"
if [[ ${fix} -eq 1 ]]; then
  (cd proto && buf format -w)
else
  (cd proto && buf format --diff --exit-code)
fi

echo "format check passed"
