#!/usr/bin/env bash
# Runs the automatable part of the definition of done (CLAUDE.md section 4) and
# prints one summary line per check. Logs go to $DOD_LOG_DIR (default: a temp
# dir). Exit status is non-zero if any check that ran failed.
#
#   .claude/skills/definition-of-done/run-dod.sh              # everything
#   PRESETS="debug tsan" .claude/skills/definition-of-done/run-dod.sh
#   BASE=origin/main .claude/skills/definition-of-done/run-dod.sh
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "${repo_root}"

presets="${PRESETS:-debug release clang-debug asan-ubsan tsan}"
base="${BASE:-origin/main}"
log_dir="${DOD_LOG_DIR:-$(mktemp -d)}"
mkdir -p "${log_dir}"

failed=0
summary=()

record() {  # record <name> <exit code> <detail>
  local status="PASS"
  if [[ "$2" -ne 0 ]]; then
    status="FAIL"
    failed=1
  fi
  summary+=("$(printf '%-28s %-4s %s' "$1" "${status}" "$3")")
}

outcome() {  # outcome <exit code> <log>: the first error on failure, else the last line
  if [[ "$1" -ne 0 ]] && grep -qm1 'error' "$2"; then
    grep -m1 'error' "$2"
  else
    tail -1 "$2"
  fi
}

skip() {  # skip <name> <reason>
  summary+=("$(printf '%-28s %-4s %s' "$1" "SKIP" "$2")")
}

# check-format.sh only sees files git tracks; mark new files as intended so a
# brand-new source file cannot slip past the format check.
git ls-files --others --exclude-standard -- exchange-core rust proto \
  | xargs -r git add -N

for preset in ${presets}; do
  log="${log_dir}/${preset}.log"
  { cmake --preset "${preset}" && cmake --build --preset "${preset}" \
      && ctest --preset "${preset}"; } >"${log}" 2>&1
  code=$?
  detail="$(grep -E 'tests passed|tests failed' "${log}" | tail -1)"
  record "ctest ${preset}" "${code}" "${detail:-see ${log}}"
done

if [[ " ${presets} " == *" debug "* ]]; then
  log="${log_dir}/architecture.log"
  ctest --preset debug -L architecture >"${log}" 2>&1
  code=$?
  record "ctest -L architecture" "${code}" "$(grep -E 'tests passed|tests failed' "${log}" | tail -1)"
fi

log="${log_dir}/format.log"
scripts/check-format.sh >"${log}" 2>&1
code=$?
record "check-format.sh" "${code}" "$(outcome "${code}" "${log}")"

log="${log_dir}/clang-tidy.log"
scripts/run-clang-tidy.sh >"${log}" 2>&1
code=$?
record "run-clang-tidy.sh" "${code}" "$(outcome "${code}" "${log}")"

changed="$(git diff --name-only "${base}"...HEAD 2>/dev/null; git diff --name-only HEAD)"

if grep -q '^rust/' <<<"${changed}"; then
  if command -v cargo >/dev/null; then
    log="${log_dir}/rust.log"
    (cd rust && cargo fmt --all -- --check \
      && cargo clippy --all-targets --locked -- -D warnings \
      && cargo test --locked) >"${log}" 2>&1
    code=$?
    record "rust fmt+clippy+test" "${code}" "see ${log}"
  else
    skip "rust fmt+clippy+test" "rust/ changed but cargo is not installed"
  fi
else
  skip "rust fmt+clippy+test" "rust/ unchanged"
fi

if grep -q '^proto/' <<<"${changed}"; then
  log="${log_dir}/proto.log"
  scripts/check-proto.sh >"${log}" 2>&1
  code=$?
  record "check-proto.sh" "${code}" "see ${log}"
else
  skip "check-proto.sh" "proto/ unchanged"
fi

if grep -qE '^(docs/|README\.md|ROADMAP\.md|CLAUDE\.md|mkdocs\.yml|requirements-docs\.txt|scripts/(docs-check|check-doc-anchors|mkdocs_hooks))' <<<"${changed}"; then
  log="${log_dir}/docs.log"
  scripts/docs-check.sh >"${log}" 2>&1
  code=$?
  record "docs-check.sh" "${code}" "$(outcome "${code}" "${log}")"
else
  skip "docs-check.sh" "docs unchanged"
fi

if grep -qE '^(exchange-core/main/|exchange-core/adapters/|rust/|proto/|scripts/e2e)' <<<"${changed}"; then
  skip "e2e-smoke.sh" "wiring may have changed: run scripts/e2e-smoke.sh after cargo build"
else
  skip "e2e-smoke.sh" "no wiring changes"
fi

echo "Definition of done (logs: ${log_dir})"
printf '%s\n' "${summary[@]}"
exit "${failed}"
