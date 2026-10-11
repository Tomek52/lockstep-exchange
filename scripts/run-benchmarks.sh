#!/usr/bin/env bash
# Builds the benchmarks with the release preset, runs all of them and prints a
# Markdown table (task 018).
#
#   scripts/run-benchmarks.sh                     # unpinned
#   LOCKSTEP_BENCH_PIN=1 scripts/run-benchmarks.sh  # pin queue/engine threads to distinct CPUs
#   scripts/run-benchmarks.sh --filter 'queue/'   # only benchmarks matching a regex
#
# Every benchmark runs with --benchmark_repetitions=5 and reports aggregates
# only; the table shows the MEDIAN over the five repetitions of each counter
# (so one noisy repetition does not decide the number). Raw JSON, including
# mean/stddev/cv, goes to build/bench-results/<executable>.json together with
# machine.json (CPU, OS, compiler, commit, command).
#
# Numbers are only meaningful from an otherwise idle machine. Not run in CI:
# shared runners are too noisy (task 018, out of scope).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bench_dir="${repo_root}/build/release/exchange-core/bench"
out_dir="${repo_root}/build/bench-results"
filter=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --filter) filter="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

executables=(lockstep_bench lockstep_bench_matching lockstep_bench_queue lockstep_bench_engine)

cd "${repo_root}"
cmake --preset release >/dev/null
cmake --build --preset release --target "${executables[@]}" >&2

rm -rf "${out_dir}"
mkdir -p "${out_dir}"

pin="${LOCKSTEP_BENCH_PIN:-0}"
args=(--benchmark_repetitions=5 --benchmark_report_aggregates_only=true
      --benchmark_format=json)
if [[ -n "${filter}" ]]; then
  args+=("--benchmark_filter=${filter}")
fi

for exe in "${executables[@]}"; do
  echo "running ${exe} ..." >&2
  LOCKSTEP_BENCH_PIN="${pin}" "${bench_dir}/${exe}" "${args[@]}" \
    --benchmark_out="${out_dir}/${exe}.json" --benchmark_out_format=json >/dev/null
done

python3 - "${repo_root}" "${out_dir}" "${pin}" "${filter}" <<'PY'
import json
import os
import platform
import re
import subprocess
import sys

repo_root, out_dir, pin, flt = sys.argv[1:5]


def run(*cmd):
    return subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True).stdout.strip()


def cpu_model():
    with open("/proc/cpuinfo") as f:
        for line in f:
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor()


compiler = run("g++-14", "--version").splitlines()[0]
command = ("LOCKSTEP_BENCH_PIN=%s scripts/run-benchmarks.sh%s"
           % (pin, (" --filter '%s'" % flt) if flt else ""))
machine = {
    "cpu": cpu_model(),
    "logical_cpus": os.cpu_count(),
    "os": platform.platform(),
    "wsl": "microsoft" in platform.release().lower(),
    "compiler": compiler,
    "commit": run("git", "rev-parse", "--short", "HEAD"),
    "dirty": bool(run("git", "status", "--porcelain", "--untracked-files=no")),
    "pinned": pin == "1",
    "command": command,
}
with open(os.path.join(out_dir, "machine.json"), "w") as f:
    json.dump(machine, f, indent=2)


def fmt_ns(value):
    if value is None:
        return "–"
    if value >= 1e6:
        return "%.2f ms" % (value / 1e6)
    if value >= 1e3:
        return "%.2f µs" % (value / 1e3)
    return "%.0f ns" % value


def fmt_rate(value):
    return "–" if value is None else "%.2f M/s" % (value / 1e6)


rows = []
for name in sorted(os.listdir(out_dir)):
    if not name.startswith("lockstep_bench") or not name.endswith(".json"):
        continue
    path = os.path.join(out_dir, name)
    if os.path.getsize(path) == 0:
        continue  # --filter matched nothing in this executable
    with open(path) as f:
        data = json.load(f)
    for b in data["benchmarks"]:
        if b.get("aggregate_name") != "median":
            continue
        rate = b.get("cmds_per_s", b.get("items_per_second"))
        rows.append((b["run_name"], b.get("p50_ns"), b.get("p99_ns"), b.get("p99.9_ns"),
                     rate, b.get("real_time"), b["time_unit"]))

print("Machine: %s, %d logical CPUs, %s%s" % (machine["cpu"], machine["logical_cpus"],
      machine["os"], " (WSL2)" if machine["wsl"] else ""))
print("Compiler: %s; threads %s" % (machine["compiler"], "pinned" if machine["pinned"] else "not pinned"))
print("Commit: %s%s" % (machine["commit"], " (+ uncommitted changes)" if machine["dirty"] else ""))
print("Command: `%s`" % machine["command"])
print()
print("| Benchmark | p50 | p99 | p99.9 | Throughput |")
print("|---|---|---|---|---|")
for run_name, p50, p99, p999, rate, _, _ in rows:
    if p50 is None:
        continue
    print("| `%s` | %s | %s | %s | %s |" % (re.sub(r"/(iterations:\d+|manual_time)", "", run_name),
          fmt_ns(p50), fmt_ns(p99), fmt_ns(p999), fmt_rate(rate)))
PY
