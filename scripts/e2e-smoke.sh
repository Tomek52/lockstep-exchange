#!/usr/bin/env bash
# End-to-end smoke test of the walking skeleton. Proves the wiring across both
# languages and all three gRPC services:
#
#   loadgen (Rust) --SubmitOrder--> exchange-core (C++) --Monitor--> risk-sentinel (Rust)
#
# Asserts: sentinel accepts the risk session, every order is acknowledged as
# accepted, exchange-core shuts down cleanly on SIGTERM, it leaves one
# journal file per shard (task 008) in a temporary journal directory, its
# --print-digest-on-exit output matches a standalone lockstep-replay run over
# that directory (task 010), and restarting it on the same directory (ADR-0020)
# works and keeps that property true after more commands.
#
# Usage: scripts/e2e-smoke.sh [--core-bin PATH] [--rust-bin-dir DIR] [--config FILE]
#   defaults: build/debug/exchange-core/main/exchange-core, rust/target/debug
#   --config FILE  start exchange-core from a JSON config (ADR-0019) instead of
#                  the --shards/--instruments flags; the file must describe the
#                  same 2 shards and instruments 1-4 for the journal assertion.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
core_bin="${repo_root}/build/debug/exchange-core/main/exchange-core"
rust_bin_dir="${repo_root}/rust/target/debug"
config_file=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-bin) core_bin="$2"; shift 2 ;;
    --rust-bin-dir) rust_bin_dir="$2"; shift 2 ;;
    --config) config_file="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
replay_bin="$(dirname "${core_bin}")/lockstep-replay"
journal_dump_bin="$(dirname "${core_bin}")/journal-dump"

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

for bin in "${core_bin}" "${replay_bin}" "${journal_dump_bin}" "${rust_bin_dir}/risk-sentinel" \
  "${rust_bin_dir}/loadgen"; do
  [[ -x "${bin}" ]] || { echo "missing binary: ${bin} (build first)" >&2; exit 1; }
done

# digest_lines_match <core.log> <label>: compares the "shard N: ... digest=..."
# lines exchange-core printed at shutdown (--print-digest-on-exit) against a
# fresh `lockstep-replay` run over the same journal directory. Both must use
# the same instrument -> shard assignment as the running exchange-core.
digest_lines_match() {
  local core_log="$1" label="$2"
  local from_core from_replay
  from_core="$(grep -E '^shard [0-9]+: ' "${core_log}")"
  if [[ -n "${config_file}" ]]; then
    from_replay="$("${replay_bin}" --journal-dir="${journal_dir}" --config="${config_file}")"
  else
    from_replay="$("${replay_bin}" --journal-dir="${journal_dir}" --shards=2 --instruments=1,2,3,4)"
  fi
  echo "    exchange-core (${label}):"
  echo "${from_core}" | sed 's/^/      /'
  echo "    lockstep-replay:"
  echo "${from_replay}" | sed 's/^/      /'
  if [[ "${from_core}" != "${from_replay}" ]]; then
    echo "digest mismatch (${label}): exchange-core vs lockstep-replay" >&2
    exit 1
  fi
}

# commands_journaled <core.log> <shard>: the "N commands" --print-digest-on-exit
# printed for that shard - this run's total record count (N1), including
# every resumed record. Used, not a hardcoded count, because the risk client
# itself broadcasts RiskLinkStatus{connected=true}/{false} as it connects and
# disconnects (on every shard, not just the one with orders), so N1 is more
# than "1 startup command + however many orders landed on this shard".
commands_journaled() {
  grep -E "^shard $2: " "$1" | sed -E 's/^shard [0-9]+: ([0-9]+) commands.*/\1/'
}

# record_is_risk_disconnected <shard.jnl> <1-based record number> <expected
# seq>: task 010 review auditor F-2's m1 - Engine::start() pushes
# RiskLinkStatus{connected=false} onto every shard's ingress before
# accepting_ is set (ADR-0020), so it must be structurally this run's very
# first record, not merely "usually first". journal-dump prints the header
# on its own line, then one line per record in file order, so record N is
# line N+1; on a resumed journal that is N1+1, not line 2 (the file's
# overall first record is from a previous run).
record_is_risk_disconnected() {
  local file="$1" record_number="$2" expected_seq="$3" line
  line="$("${journal_dump_bin}" "${file}" | sed -n "$((record_number + 1))p")"
  echo "    ${file} record ${record_number}: ${line}"
  if [[ "${line}" != "seq=${expected_seq} ts="*" RiskLinkStatus connected=false" ]]; then
    echo "record ${record_number} of ${file} is not 'seq=${expected_seq} ... RiskLinkStatus connected=false': ${line}" >&2
    exit 1
  fi
}

echo "==> starting risk-sentinel on :${sentinel_port}"
RUST_LOG=info "${rust_bin_dir}/risk-sentinel" --listen "127.0.0.1:${sentinel_port}" \
  >"${logs}/sentinel.log" 2>&1 &
sentinel_pid=$!
wait_for_log "${logs}/sentinel.log" "risk-sentinel listening" "sentinel to listen"

echo "==> starting exchange-core on :${core_port}"
# --config supplies shards and instruments (ADR-0019); otherwise fall back to
# the equivalent flags. Both paths describe 2 shards and instruments 1-4.
if [[ -n "${config_file}" ]]; then
  echo "    using config file: ${config_file}"
  core_instrument_args=(--config="${config_file}")
else
  core_instrument_args=(--shards=2 --instruments=1,2,3,4)
fi
"${core_bin}" --listen="127.0.0.1:${core_port}" --risk-sentinel="127.0.0.1:${sentinel_port}" \
  "${core_instrument_args[@]}" --journal-dir="${journal_dir}" --print-digest-on-exit \
  >"${logs}/core.log" 2>&1 &
core_pid=$!
wait_for_log "${logs}/core.log" "exchange-core listening" "exchange-core to listen"

echo "==> risk session handshake"
wait_for_log "${logs}/sentinel.log" "session established" "sentinel to accept the session"
wait_for_log "${logs}/core.log" "session accepted by sentinel" "exchange-core to see the accept"

echo "==> submitting orders via loadgen"
"${rust_bin_dir}/loadgen" --target "http://127.0.0.1:${core_port}" \
  --scenario single --count 5 --instrument 2 --expect-accepted

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

echo "==> first record of each shard journal is RiskLinkStatus connected=false (m1)"
# Every shard's own first record this run, regardless of whether any order
# landed on it (ADR-0020 step 6, task 010 review auditor F-2/m1).
record_is_risk_disconnected "${journal_dir}/shard-0.jnl" 1 1
record_is_risk_disconnected "${journal_dir}/shard-1.jnl" 1 1

echo "==> lockstep-replay digest vs --print-digest-on-exit (first run)"
digest_lines_match "${logs}/core.log" "first run"

echo "==> restarting exchange-core on the same journal dir (ADR-0020)"
"${core_bin}" --listen="127.0.0.1:${core_port}" --risk-sentinel="127.0.0.1:${sentinel_port}" \
  "${core_instrument_args[@]}" --journal-dir="${journal_dir}" --print-digest-on-exit \
  >"${logs}/core-restart.log" 2>&1 &
core_pid=$!
wait_for_log "${logs}/core-restart.log" "exchange-core listening" "restarted exchange-core to listen"
wait_for_log "${logs}/sentinel.log" "session established" "sentinel to accept the restarted session"
wait_for_log "${logs}/core-restart.log" "session accepted by sentinel" \
  "restarted exchange-core to see the accept"

echo "==> submitting more orders after restart"
# Different --trader than the first run: the first run's orders are still
# resting (nothing cancelled them), and loadgen reuses the same client order
# ids on every invocation, so resubmitting as the same trader would hit
# RejectReason::DuplicateClientOrderId - itself proof the restart kept state,
# but not what this step is checking.
"${rust_bin_dir}/loadgen" --target "http://127.0.0.1:${core_port}" \
  --scenario single --count 5 --trader 2 --instrument 2 --expect-accepted

echo "==> graceful shutdown (restart)"
kill -TERM "${core_pid}"
wait "${core_pid}"
core_pid=""
grep -q "exchange-core stopped cleanly" "${logs}/core-restart.log"

echo "==> first record this run is RiskLinkStatus connected=false, after the resumed ones (m1)"
# N1 (how many records the first run left on each shard) comes from its own
# --print-digest-on-exit output, not a hardcoded count: the risk client's
# own connect/disconnect broadcasts land on every shard, including shard 0,
# which no order ever reaches. This run's startup command must be exactly
# the next record after those N1, not merely present somewhere in the file.
shard0_n1="$(commands_journaled "${logs}/core.log" 0)"
shard1_n1="$(commands_journaled "${logs}/core.log" 1)"
record_is_risk_disconnected "${journal_dir}/shard-0.jnl" "$((shard0_n1 + 1))" "$((shard0_n1 + 1))"
record_is_risk_disconnected "${journal_dir}/shard-1.jnl" "$((shard1_n1 + 1))" "$((shard1_n1 + 1))"

echo "==> lockstep-replay digest vs --print-digest-on-exit (after restart)"
digest_lines_match "${logs}/core-restart.log" "after restart"

kill -TERM "${sentinel_pid}"
wait "${sentinel_pid}" || true
sentinel_pid=""

echo "E2E SMOKE PASSED"
