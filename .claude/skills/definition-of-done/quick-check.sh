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
# QC_MAX_LINES (default 60): cap on the failure excerpt.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "${repo_root}"

preset="${PRESET:-debug}"
log_dir="${QC_LOG_DIR:-${repo_root}/build/logs}"
max_lines="${QC_MAX_LINES:-60}"
mkdir -p "${log_dir}"
build_log="${log_dir}/${preset}-build.log"
test_log="${log_dir}/${preset}-test.log"
# Truncate: the build step appends after configure, and a stale error from an
# earlier run must never show up as a current one.
: >"${build_log}"

# The build tree re-runs CMake by itself when a CMakeLists.txt changes, so
# configure only when the tree does not exist yet.
if [[ ! -f "build/${preset}/CMakeCache.txt" ]]; then
  if ! cmake --preset "${preset}" >"${build_log}" 2>&1; then
    echo "CONFIGURE FAILED (${preset}); full log: ${build_log}"
    grep -m "${max_lines}" -E -A3 'CMake Error' "${build_log}" || tail -n 20 "${build_log}"
    exit 1
  fi
fi

if ! cmake --build --preset "${preset}" >>"${build_log}" 2>&1; then
  echo "BUILD FAILED (${preset}); full log: ${build_log}"
  # The "FAILED:" line names the target; the full compiler command after it is noise.
  grep -E '^FAILED: ' "${build_log}" | cut -c1-200
  grep -E -A3 'error:|undefined reference|CMake Error' "${build_log}" \
    | head -n "${max_lines}"
  exit 1
fi

if ctest --preset "${preset}" "$@" >"${test_log}" 2>&1; then
  echo "PASS (${preset}): $(grep -E 'tests passed' "${test_log}" | tail -1)"
  exit 0
fi

echo "TESTS FAILED (${preset}); full log: ${test_log}"
grep -E 'tests passed|tests failed|No tests were found' "${test_log}" | tail -1
# gtest prints each failed assertion as "file:line: Failure" followed by the
# expected and actual values; sanitizer reports start with WARNING/ERROR lines.
{
  grep -E -A6 ': Failure$' "${test_log}"
  grep -E -A12 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:' "${test_log}"
} | head -n "${max_lines}"
sed -n '/The following tests FAILED/,$p' "${test_log}" | head -n 20
exit 1
