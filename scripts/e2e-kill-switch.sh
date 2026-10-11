#!/usr/bin/env bash
# End-to-end risk-loop scenario (task 019): the whole feedback loop of
# ADR-0013 across processes and languages.
#
#   loadgen (Rust) --SubmitOrder--> exchange-core (C++) <--Monitor--> risk-sentinel (Rust)
#
# 1. a trader breaches the position limit (loadgen `breach`);
# 2. risk-sentinel blocks them (BlockTrader sent, CommandApplied received);
# 3. exchange-core rejects them with TRADER_BLOCKED while other traders keep
#    getting accepts (a concurrent loadgen `random` run);
# 3b. a buy that sweeps two resting sells (one command, two trades) is blocked
#    only if the sentinel counts both fills (task 021, ADR-0022);
# 4. a falling market of many moderate losses (loadgen `loss`) pushes the
#    gross loss over the limit (ADR-0021): the sentinel engages the kill
#    switch and every later order, from any trader, gets TRADING_HALTED;
# 5. after stopping exchange-core, lockstep-replay over the journals gives the
#    same digests as the live --print-digest-on-exit output, so every
#    rejection is reproduced exactly (ADR-0004);
# 6. journal-dump shows the BlockTrader and KillSwitch commands in every shard.
#
# Limits (see the comments at SENTINEL_LIMITS): the position limit is hit by
# `breach` only, the per-trader loss limit by nobody, and the kill-switch limit
# is an order of magnitude above the swings of the random traders, so only the
# `loss` scenario trips it.
#
# Instruments: 1-5, declared once below. 1-4 are the ones `config/exchange.json`
# and the compose stack use; 5 is private to the sweep step (task 021), whose
# traders must not move any other mark. This script passes --instruments
# itself and does not read the config file (e2e-smoke.sh --config does).
#
# Ports: two free ports are picked at start (task 022), so concurrent runs and
# a stale process on a fixed port cannot interfere. CORE_PORT / SENTINEL_PORT
# override them; a port that is already taken stops the script before anything
# starts.
#
# Usage: scripts/e2e-kill-switch.sh [--core-bin PATH] [--rust-bin-dir DIR]
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
replay_bin="$(dirname "${core_bin}")/lockstep-replay"
journal_dump_bin="$(dirname "${core_bin}")/journal-dump"
loadgen_bin="${rust_bin_dir}/loadgen"

# ---------------------------------------------------------------- checks first
# Everything here runs before the first process starts and before the log
# directory exists, so a refusal prints one line and nothing else.
instruments=(1 2 3 4 5)
instruments_csv="$(IFS=,; echo "${instruments[*]}")"
breach_instrument=2
loss_instrument=1
random_instruments=3,4
sweep_instrument=5

require_instrument() {  # <id> <what>
  local id
  for id in "${instruments[@]}"; do
    [[ "${id}" == "$1" ]] && return 0
  done
  echo "instrument $1 ($2) is not in --instruments ${instruments_csv}" >&2
  exit 1
}
require_instrument "${breach_instrument}" breach
require_instrument "${loss_instrument}" loss
require_instrument "${sweep_instrument}" sweep
for id in ${random_instruments//,/ }; do require_instrument "${id}" random; done

# Prints <n> distinct free TCP ports of 127.0.0.1, one per line: all sockets
# are bound before any is read, so the ports differ. Between this and the
# process binding the port another process could take it; the script then
# fails on its own checks instead of succeeding.
free_ports() {
  python3 - "$1" <<'PY'
import socket, sys
socks = [socket.socket() for _ in range(int(sys.argv[1]))]
for s in socks:
    s.bind(("127.0.0.1", 0))
for s in socks:
    print(s.getsockname()[1])
PY
}

default_ports=(15061 15062)  # without python3
if command -v python3 >/dev/null 2>&1; then
  mapfile -t picked < <(free_ports 2 2>/dev/null)
  if (( ${#picked[@]} == 2 )); then default_ports=("${picked[@]}"); fi
fi
core_port="${CORE_PORT:-${default_ports[0]}}"
sentinel_port="${SENTINEL_PORT:-${default_ports[1]}}"

# True if something accepts connections on 127.0.0.1:<port>.
port_in_use() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
require_port_free() {  # <port> <what>
  if port_in_use "$1"; then
    echo "port $1 ($2) is already in use; stop that process or set ${3} to a free port" >&2
    exit 1
  fi
}
require_port_free "${sentinel_port}" risk-sentinel SENTINEL_PORT
require_port_free "${core_port}" exchange-core CORE_PORT
target="http://127.0.0.1:${core_port}"
logs="$(mktemp -d)"
journal_dir="${logs}/journal"
core_pid=""
sentinel_pid=""
random_pid=""

# Limits, in the sentinel's units (lots, tick-lots).
#  - max-abs-position 2000: `breach` adds 100 lots a round, so it trips after 21
#    rounds; the random traders (a few hundred orders of at most 100 lots over
#    two instruments) stay far below.
#  - max-trader-loss 50000: no single trader of any scenario gets near it (the
#    biggest single loss in `loss` is ~6 400), so only the aggregate can halt.
#  - kill-switch-loss 100000 (gross loss, ADR-0021): `loss` reaches it after
#    about 33 rounds; the unrealised swings of the four random traders add up
#    to a few tens of thousands at most.
sentinel_limits=(--max-abs-position 2000 --max-trader-loss 50000 --kill-switch-loss 100000)

cleanup() {
  local status=$?
  # Stop the exchange before the sentinel: the sentinel's graceful shutdown
  # waits for the Monitor stream to close. Waiting leaves no process behind.
  [[ -n "${random_pid}" ]] && { kill "${random_pid}" 2>/dev/null || true; wait "${random_pid}" 2>/dev/null || true; }
  [[ -n "${core_pid}" ]] && { kill "${core_pid}" 2>/dev/null || true; wait "${core_pid}" 2>/dev/null || true; }
  [[ -n "${sentinel_pid}" ]] && { kill "${sentinel_pid}" 2>/dev/null || true; wait "${sentinel_pid}" 2>/dev/null || true; }
  if [[ ${status} -ne 0 ]]; then
    for out in breach random loss; do
      [[ -f "${logs}/${out}.out" ]] && { echo "---- loadgen ${out} output ----"; cat "${logs}/${out}.out"; }
    done
    echo "---- exchange-core log ----"; cat "${logs}/core.log" 2>/dev/null || true
    echo "---- risk-sentinel log ----"; cat "${logs}/sentinel.log" 2>/dev/null || true
    echo "E2E KILL-SWITCH FAILED"
  fi
  rm -rf "${logs}"
  exit ${status}
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# wait_for_log <file> <ERE pattern> <what> [seconds]
wait_for_log() {
  local tries=$(( ${4:-10} * 10 ))
  for _ in $(seq 1 "${tries}"); do
    if grep -Eq "$2" "$1" 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "timed out waiting for: $3" >&2
  return 1
}

for bin in "${core_bin}" "${replay_bin}" "${journal_dump_bin}" "${rust_bin_dir}/risk-sentinel" \
  "${loadgen_bin}"; do
  [[ -x "${bin}" ]] || { echo "missing binary: ${bin} (build first)" >&2; exit 1; }
done

# The number printed after "<label>: " on a loadgen output line, e.g. 4698.
# field <file> <ERE with one group>
field() { sed -nE "s/$2/\\1/p" "$1" | head -n 1; }

# count_reason <file> <REJECT_REASON_X>: how many orders that run had rejected
# with that reason (0 if the line is absent).
count_reason() {
  local n
  n="$(field "$1" "^  rejected $2: ([0-9]+)\$")"
  echo "${n:-0}"
}

export NO_COLOR=1  # plain log lines for grep (tracing honours NO_COLOR)

echo "==> starting risk-sentinel on :${sentinel_port} (${sentinel_limits[*]})"
RUST_LOG=info "${rust_bin_dir}/risk-sentinel" --listen "127.0.0.1:${sentinel_port}" \
  "${sentinel_limits[@]}" >"${logs}/sentinel.log" 2>&1 &
sentinel_pid=$!
wait_for_log "${logs}/sentinel.log" "risk-sentinel listening" "sentinel to listen"

echo "==> starting exchange-core on :${core_port}"
"${core_bin}" --listen="127.0.0.1:${core_port}" --risk-sentinel="127.0.0.1:${sentinel_port}" \
  --shards=2 --instruments="${instruments_csv}" --journal-dir="${journal_dir}" --print-digest-on-exit \
  >"${logs}/core.log" 2>&1 &
core_pid=$!
wait_for_log "${logs}/core.log" "exchange-core listening" "exchange-core to listen"
wait_for_log "${logs}/sentinel.log" "session established" "sentinel to accept the session"
wait_for_log "${logs}/core.log" "session accepted by sentinel" "exchange-core to see the accept"

# ---------------------------------------------------------------- 1-3: block
echo "==> traders 2-5 trade instruments ${random_instruments} in the background (random)"
# Instruments dealt round-robin by id: 1,3 on shard 0 and 2,4 on shard 1, so the
# random flow shares a shard with the breach (instrument 2) but not its book.
"${loadgen_bin}" --target "${target}" --scenario random --trader 2 --traders 4 \
  --concurrency 4 --instruments "${random_instruments}" --count 400 --rate 100 \
  >"${logs}/random.out" 2>&1 &
random_pid=$!
sleep 0.5

echo "==> trader 666 breaches the position limit on instrument ${breach_instrument} (breach)"
"${loadgen_bin}" --target "${target}" --scenario breach --instrument "${breach_instrument}" >"${logs}/breach.out" 2>&1 \
  || fail "loadgen breach did not see TRADER_BLOCKED"
cat "${logs}/breach.out"
if kill -0 "${random_pid}" 2>/dev/null; then
  echo "    (the random run was still going when 666 was blocked)"
else
  echo "    note: the random run finished before the breach ended; overlap was short"
fi

echo "==> sentinel sent BlockTrader for 666 and the exchange applied it"
wait_for_log "${logs}/sentinel.log" "risk command sent.*BlockTrader \{ trader: 666" \
  "sentinel to send BlockTrader for 666"
wait_for_log "${logs}/sentinel.log" "risk command applied by exchange.*BlockTrader \{ trader: 666" \
  "CommandApplied for the BlockTrader of 666"
grep -E "risk command (sent|applied by exchange).*BlockTrader" "${logs}/sentinel.log" \
  | sed 's/^/    /'

echo "==> the random traders kept getting accepts"
random_status=0
wait "${random_pid}" || random_status=$?
random_pid=""
cat "${logs}/random.out"
[[ ${random_status} -eq 0 ]] || fail "loadgen random exited with ${random_status}"
random_accepted="$(field "${logs}/random.out" '^accepted=([0-9]+) .*')"
(( ${random_accepted:-0} > 0 )) || fail "the random traders got no accepts"
for reason in REJECT_REASON_TRADER_BLOCKED REJECT_REASON_TRADING_HALTED; do
  (( $(count_reason "${logs}/random.out" "${reason}") == 0 )) \
    || fail "a random trader was rejected with ${reason}"
done

# ---------------------------------------------------------------- 3b: sweep
# Task 021 / ADR-0022. Instrument 5 is nobody else's, so its mark never moves
# and the two traders below end with a PnL of zero (the loss scenario must be
# the only thing that raises the gross loss). Two resting sells of 1 500 lots
# and one buy of 3 000 that takes both: one command, two trades, one shard
# sequence. Each fill alone is under --max-abs-position (2 000); only the sum
# is over it, so the BlockTrader below exists only if the sentinel accounts for
# the second trade as well.
echo "==> a buy that sweeps two resting sells on instrument ${sweep_instrument} is fully accounted for"
"${loadgen_bin}" --target "${target}" --scenario single --instrument "${sweep_instrument}" --price 100 --quantity 1500 \
  --side sell --trader 9001 --count 2 --expect-accepted >"${logs}/sweep-sells.out" 2>&1 \
  || { cat "${logs}/sweep-sells.out"; fail "the two resting sells were not accepted"; }
"${loadgen_bin}" --target "${target}" --scenario single --instrument "${sweep_instrument}" --price 100 --quantity 3000 \
  --side buy --trader 9002 --count 1 --expect-accepted >"${logs}/sweep-buy.out" 2>&1 \
  || { cat "${logs}/sweep-buy.out"; fail "the sweeping buy was not accepted"; }
wait_for_log "${logs}/sentinel.log" "risk command sent.*BlockTrader \{ trader: 9002" \
  "sentinel to block 9002 after the sweep (both fills counted)"
wait_for_log "${logs}/sentinel.log" "risk command applied by exchange.*BlockTrader \{ trader: 9002" \
  "CommandApplied for the BlockTrader of 9002"
grep -E "risk command (sent|applied by exchange).*BlockTrader \{ trader: 9002" "${logs}/sentinel.log" \
  | sed 's/^/    /'

# ---------------------------------------------------------------- 4: halt
echo "==> a falling market of many moderate losses on instrument ${loss_instrument} (loss)"
"${loadgen_bin}" --target "${target}" --scenario loss --instrument "${loss_instrument}" >"${logs}/loss.out" 2>&1 \
  || fail "loadgen loss did not see TRADING_HALTED"
cat "${logs}/loss.out"

echo "==> sentinel engaged the kill switch and the exchange applied it on every shard"
wait_for_log "${logs}/sentinel.log" "risk command sent.*KillSwitch \{ engaged: true" \
  "sentinel to send the KillSwitch"
# CommandApplied is sent once all shards have applied the command (ADR-0013).
wait_for_log "${logs}/sentinel.log" "risk command applied by exchange.*KillSwitch" \
  "CommandApplied for the KillSwitch" 15
grep -E "risk command (sent|applied by exchange).*KillSwitch" "${logs}/sentinel.log" \
  | sed 's/^/    /'

echo "==> every later order, from any trader, on every instrument, is rejected with TRADING_HALTED"
probe=0
for instrument in "${instruments[@]}"; do
  # Trader 2 traded earlier and is not blocked; trader 4242 never traded.
  for trader in 2 4242; do
    probe=$((probe + 1))
    "${loadgen_bin}" --target "${target}" --scenario single --count 3 --trader "${trader}" \
      --instrument "${instrument}" --client-id-offset 900000 --expect-rejected trading-halted \
      >"${logs}/probe-${probe}.out" 2>&1 \
      || { cat "${logs}/probe-${probe}.out"; fail "trader ${trader} on instrument ${instrument} was not halted"; }
    echo "    trader ${trader}, instrument ${instrument}: 3 of 3 rejected with TRADING_HALTED"
  done
done

# ---------------------------------------------------------------- 5: replay
echo "==> graceful shutdown"
kill -TERM "${core_pid}"
wait "${core_pid}"
core_pid=""
grep -q "exchange-core stopped cleanly" "${logs}/core.log"

echo "==> lockstep-replay digest vs --print-digest-on-exit"
from_core="$(grep -E '^shard [0-9]+: ' "${logs}/core.log")"
from_replay="$("${replay_bin}" --journal-dir="${journal_dir}" --shards=2 --instruments="${instruments_csv}")"
echo "    exchange-core:"; echo "${from_core}" | sed 's/^/      /'
echo "    lockstep-replay:"; echo "${from_replay}" | sed 's/^/      /'
[[ -n "${from_core}" && "${from_core}" == "${from_replay}" ]] \
  || fail "digest mismatch: exchange-core vs lockstep-replay"

# ---------------------------------------------------------------- 6: journals
echo "==> journals contain the BlockTrader and KillSwitch commands on every shard"
for shard in 0 1; do
  dump="$("${journal_dump_bin}" "${journal_dir}/shard-${shard}.jnl")"
  echo "${dump}" | grep -E "BlockTrader command_id=[0-9]+ trader=666$" | sed "s/^/    shard ${shard}: /" \
    || fail "shard ${shard} journal has no BlockTrader for trader 666"
  echo "${dump}" | grep -E "KillSwitch command_id=[0-9]+ engaged=true$" | sed "s/^/    shard ${shard}: /" \
    || fail "shard ${shard} journal has no KillSwitch engaged=true"
done

# ---------------------------------------------------------------- timeline
block_us="$(field "${logs}/breach.out" '^first fill to TRADER_BLOCKED: ([0-9]+) us.*')"
halt_us="$(field "${logs}/loss.out" '^first fill to TRADING_HALTED: ([0-9]+) us.*')"
fills="$(field "${logs}/loss.out" '^first fill to TRADING_HALTED: [0-9]+ us \(([0-9]+) fills\)$')"
echo
echo "==> scenario timeline"
echo "    time to block (first fill -> TRADER_BLOCKED, breach):  ${block_us:-n/a} us"
echo "    time to halt  (first fill -> TRADING_HALTED, loss):    ${halt_us:-n/a} us (${fills:-?} fills)"
echo "    rejected orders by reason, all loadgen runs:"
cat "${logs}"/*.out \
  | sed -nE 's/^  rejected (REJECT_REASON_[A-Z_]+|UNKNOWN_[0-9]+): ([0-9]+)$/\1 \2/p' \
  | awk '{ n[$1] += $2 } END { for (k in n) printf "      %-32s %d\n", k, n[k] }' | sort

# The two reasons this scenario is about must both have been seen.
all_outputs="$(cat "${logs}"/*.out)"
grep -q "rejected REJECT_REASON_TRADER_BLOCKED" <<<"${all_outputs}" || fail "no TRADER_BLOCKED rejection seen"
grep -q "rejected REJECT_REASON_TRADING_HALTED" <<<"${all_outputs}" || fail "no TRADING_HALTED rejection seen"

kill -TERM "${sentinel_pid}"
wait "${sentinel_pid}" || true
sentinel_pid=""

echo "E2E KILL-SWITCH PASSED"
