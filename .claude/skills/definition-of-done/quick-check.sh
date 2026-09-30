#!/usr/bin/env bash
# Inner-loop build and test for one preset. Keeps the full output in log files
# and prints only what an agent needs to act on: one line when green, the
# compiler errors or failing tests when red. Raw build and test logs are the
# largest avoidable cost in an agent's context (ADR-0016). This is not the
# definition of done; run-dod.sh is.
#
#   .claude/skills/definition-of-done/quick-check.sh                # debug, all tests
#   .claude/skills/definition-of-done/quick-check.sh -R Matching    # extra args go to ctest
#   PRESET=tsan .claude/skills/definition-of-done/quick-check.sh -L unit
#
# Knobs: PRESET (default debug), QC_LOG_DIR (default build/logs, gitignored),
# QC_MAX_LINES (default 60): cap on compiler errors and on failed assertions,
# QC_MAX_SANITIZER_LINES (default 200): cap on sanitizer reports.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "${repo_root}"

preset="${PRESET:-debug}"
log_dir="${QC_LOG_DIR:-${repo_root}/build/logs}"
max_lines="${QC_MAX_LINES:-60}"
mkdir -p "${log_dir}"
build_log="${log_dir}/${preset}-build.log"
test_log="${log_dir}/${preset}-test.log"
# Truncate both: a stale error, or a stale green test log after a failed
# build, must never be read as the current result. Parallel agents on the same
# preset share a build tree anyway; give each its own QC_LOG_DIR if they must
# run at the same time.
: >"${build_log}"
: >"${test_log}"

excerpt_or_tail() {  # excerpt_or_tail <log> <excerpt>: the excerpt, or the log's end when it is empty
  if [[ -n "$2" ]]; then
    printf '%s\n' "$2"
  else
    tail -n 20 "$1"
  fi
}

# The build tree re-runs CMake by itself when a CMakeLists.txt changes, but
# not when a preset's cache variables change, so configure when the tree is
# missing or older than CMakePresets.json.
cache="build/${preset}/CMakeCache.txt"
if [[ ! -f "${cache}" || CMakePresets.json -nt "${cache}" ]]; then
  if ! cmake --preset "${preset}" >"${build_log}" 2>&1; then
    echo "CONFIGURE FAILED (${preset}); full log: ${build_log}"
    excerpt_or_tail "${build_log}" \
      "$(grep -E -A3 'CMake Error' "${build_log}" | head -n "${max_lines}")"
    exit 1
  fi
fi

if ! cmake --build --preset "${preset}" >>"${build_log}" 2>&1; then
  echo "BUILD FAILED (${preset}); full log: ${build_log}"
  # The "FAILED:" line names the target; the full compiler command after it is noise.
  grep -E '^FAILED: ' "${build_log}" | cut -c1-200
  excerpt_or_tail "${build_log}" \
    "$(grep -E -A3 'error:|undefined reference|CMake Error' "${build_log}" | head -n "${max_lines}")"
  exit 1
fi

if ctest --preset "${preset}" "$@" >"${test_log}" 2>&1; then
  echo "PASS (${preset}): $(grep -E 'tests passed' "${test_log}" | tail -1)"
  exit 0
fi

echo "TESTS FAILED (${preset}); full log: ${test_log}"
grep -E 'tests passed|tests failed|No tests were found' "${test_log}" | tail -1
# gtest prints each failed assertion as "file:line: Failure" followed by the
# expected and actual values.
assertions="$(grep -E -A6 ': Failure$' "${test_log}" | head -n "${max_lines}")"
# A sanitizer report runs from its WARNING/ERROR/FATAL header to its SUMMARY
# line; a TSan race needs both stacks (this access and the previous one), so
# print whole reports under their own cap rather than a fixed number of lines.
sanitizers="$(awk '
  /(WARNING|ERROR|FATAL): [A-Za-z]+Sanitizer|runtime error:/ { inside = 1 }
  inside { print }
  /^SUMMARY: [A-Za-z]+Sanitizer/ { inside = 0 }
' "${test_log}" | head -n "${QC_MAX_SANITIZER_LINES:-200}")"
excerpt_or_tail "${test_log}" "${assertions}${assertions:+${sanitizers:+$'\n'}}${sanitizers}"
sed -n '/The following tests FAILED/,$p' "${test_log}" | head -n 20
exit 1
