#!/usr/bin/env bash
# End-to-end smoke test of the walking skeleton. Proves the wiring across both
# languages and all three gRPC services:
#
#   loadgen (Rust) --SubmitOrder--> exchange-core (C++) --Monitor--> risk-sentinel (Rust)
#
# Asserts: sentinel accepts the risk session, every order is acknowledged as
# accepted, exchange-core shuts down cleanly on SIGTERM, and it leaves one
# journal file per shard (task 008) in a temporary journal directory.
#
# Usage: scripts/e2e-smoke.sh [--core-bin PATH] [--rust-bin-dir DIR]
#   defaults: build/debug/exchange-core/main/exchange-core, rust/target/debug
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
core_bin="${repo_root}/build/debug/exchange-core/main/exchange-core"
rust_bin_dir="${repo_root}/rust/target/debug"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-bin) core_bin="$2"; shift 2 ;;
    --rust-bin-dir) rust_bin_dir="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

core_port="${CORE_PORT:-15051}"
sentinel_port="${SENTINEL_PORT:-15052}"
logs="$(mktemp -d)"
journal_dir="${logs}/journal"
core_pid=""
sentinel_pid=""

cleanup() {
  local status=$?
  [[ -n "${core_pid}" ]] && kill "${core_pid}" 2>/dev/null || true
  [[ -n "${sentinel_pid}" ]] && kill "${sentinel_pid}" 2>/dev/null || true
  if [[ ${status} -ne 0 ]]; then
    echo "---- exchange-core log ----"; cat "${logs}/core.log" 2>/dev/null || true
    echo "---- risk-sentinel log ----"; cat "${logs}/sentinel.log" 2>/dev/null || true
    echo "E2E SMOKE FAILED"
  fi
  rm -rf "${logs}"
  exit ${status}
}
trap cleanup EXIT

# wait_for_log <file> <pattern> <what>
wait_for_log() {
  for _ in $(seq 1 100); do
    if grep -q "$2" "$1" 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "timed out waiting for: $3" >&2
  return 1
}

for bin in "${core_bin}" "${rust_bin_dir}/risk-sentinel" "${rust_bin_dir}/loadgen"; do
  [[ -x "${bin}" ]] || { echo "missing binary: ${bin} (build first)" >&2; exit 1; }
done

echo "==> starting risk-sentinel on :${sentinel_port}"
RUST_LOG=info "${rust_bin_dir}/risk-sentinel" --listen "127.0.0.1:${sentinel_port}" \
  >"${logs}/sentinel.log" 2>&1 &
sentinel_pid=$!
wait_for_log "${logs}/sentinel.log" "risk-sentinel listening" "sentinel to listen"

echo "==> starting exchange-core on :${core_port}"
"${core_bin}" --listen="127.0.0.1:${core_port}" --risk-sentinel="127.0.0.1:${sentinel_port}" \
  --shards=2 --instruments=1,2,3,4 --journal-dir="${journal_dir}" >"${logs}/core.log" 2>&1 &
core_pid=$!
wait_for_log "${logs}/core.log" "exchange-core listening" "exchange-core to listen"

echo "==> risk session handshake"
wait_for_log "${logs}/sentinel.log" "session established" "sentinel to accept the session"
wait_for_log "${logs}/core.log" "session accepted by sentinel" "exchange-core to see the accept"

echo "==> submitting orders via loadgen"
"${rust_bin_dir}/loadgen" --target "http://127.0.0.1:${core_port}" \
  --count 5 --instrument 2 --expect-accepted

echo "==> graceful shutdown"
kill -TERM "${core_pid}"
wait "${core_pid}"
core_pid=""
grep -q "exchange-core stopped cleanly" "${logs}/core.log"

echo "==> journal files"
journal_header_size=32  # FileHeader, ADR-0012
for shard in 0 1; do
  file="${journal_dir}/shard-${shard}.jnl"
  [[ -f "${file}" ]] || { echo "missing journal: ${file}" >&2; exit 1; }
  echo "    shard-${shard}.jnl: $(stat -c %s "${file}") bytes"
done
# Instruments are dealt round-robin by id, so instrument 2 lives on shard 1:
# the loadgen orders must have been journaled there.
if (( $(stat -c %s "${journal_dir}/shard-1.jnl") <= journal_header_size )); then
  echo "shard-1.jnl holds no records" >&2
  exit 1
fi
kill -TERM "${sentinel_pid}"
wait "${sentinel_pid}" || true
sentinel_pid=""

echo "E2E SMOKE PASSED"
