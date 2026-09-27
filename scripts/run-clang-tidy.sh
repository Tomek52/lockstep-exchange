#!/usr/bin/env bash
# clang-tidy over every translation unit we own, using the compile database of
# the clang-debug preset. Generated protobuf code and third-party headers are
# excluded (.clang-tidy HeaderFilterRegex, and the file regex below).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build/clang-debug"
cd "${repo_root}"

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
  cmake --preset clang-debug >/dev/null
fi
# Generated headers must exist before tidy can parse adapters that include them.
cmake --build --preset clang-debug --target lockstep_proto >/dev/null

run-clang-tidy-19 -p "${build_dir}" -quiet -j "$(nproc)" \
  "${repo_root}/exchange-core/(domain|app|adapters|main|concurrency)/.*\.cpp$"
echo "clang-tidy passed"
