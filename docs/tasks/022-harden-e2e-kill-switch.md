# 022: Harden the end-to-end kill-switch script

## Goal

Make `scripts/e2e-kill-switch.sh` fail clearly, or not at all, when its
environment is not the clean one it assumes: pick free TCP ports instead of
two fixed ones, refuse early and by name to start on a port that is already
taken, and state the instruments it needs in one place that is checked
against every scenario that uses one.

## Context

- [Task 019](019-e2e-kill-switch.md) added the script and
  [task 021](021-execution-report-trade-index.md) added its instrument-5 sweep
  step. The 019 review (PR #27) listed two non-blocking risks; a third came
  from the 021 review (PR #28). This task measures each one before changing
  anything:
  1. **Fixed ports.** `core_port` and `sentinel_port` default to 15061 and
     15062 (`CORE_PORT` and `SENTINEL_PORT` override them).
     *Measured on 2026-10-11:* with a stale listener on 15062 the script fails,
     after 11 s, with `timed out waiting for: sentinel to accept the session`
     and a log that still says `risk-sentinel listening ... 127.0.0.1:15062`
     (the sentinel logs "listening" before the bind error, which only appears
     two lines later). It fails closed but points at the wrong thing. Two
     concurrent runs on one host (two worktrees, a self-hosted runner)
     collide the same way. `scripts/e2e-smoke.sh` has the same pattern with
     15051 and 15052 and is **not** changed here.
  2. **Probe orders that meet back-pressure.** Already handled, nothing to
     change: `loadgen` retries `RESOURCE_EXHAUSTED` and `OVERLOADED` up to
     `MAX_RETRIES = 100` times with a jittered back-off capped at 100 ms
     (`execute` in `rust/crates/lockstep-loadgen/src/main.rs`), roughly ten
     seconds, before it counts the operation as `resource_exhausted`. The
     review suggestion was made without reading that code.
  3. **CPU load.** *Measured on 2026-10-11, 8-core WSL2 host, debug build:*
     the script passed 5 of 5 runs with 8 busy loops and 6 of 6 runs with 32
     busy loops (4x oversubscribed); a run took 8-10 s instead of about 5 s
     and "time to block" rose from 0.17 s to 1.3 s. No flake observed, so no
     timeout is changed. The acceptance criteria below repeat the
     measurement after the change.
  4. **Instrument ids spread over the script.** `--instruments=1,2,3,4,5` is
     written in two places (the exchange and `lockstep-replay`), and each
     scenario hard-codes the id it trades (`--instrument 2`, `1`, `5`,
     `--instruments 3,4`, probes over `1 2 3 4`). `config/exchange.json` and
     `deploy/docker-compose.yml` list only 1-4 and are deliberately not used
     by this script (it passes `--instruments` explicitly; `e2e-smoke.sh
     --config` is the one that uses the file). Nothing says so, and nothing
     fails early if a scenario names an instrument the exchange was not
     started with: the symptom would be rejections far from the cause.
- The scripts are tested by running them; there is no unit-test harness for
  shell. Each criterion below names the command that shows it.

## Interfaces to implement

No production code. In `scripts/e2e-kill-switch.sh`:

```bash
# Ports: CORE_PORT / SENTINEL_PORT win if set; otherwise two free ports
# picked by binding port 0 on 127.0.0.1 (python3). Without python3 the old
# defaults 15061 / 15062 apply.
free_ports 2

# Exits 1 with "port N (WHAT) is already in use" if something accepts
# connections on 127.0.0.1:N. Called for both ports before anything starts.
require_port_free <port> <what>

# One declaration; every scenario id is derived from it or checked against it.
instruments=(1 2 3 4 5)
require_instrument <id> <what>    # fails early when <id> is not in $instruments
```

The exchange and `lockstep-replay` get `--instruments=$(IFS=,; echo
"${instruments[*]}")`. The scenario ids become named variables
(`breach_instrument=2`, `loss_instrument=1`, `random_instruments=3,4`,
`sweep_instrument=5`) checked with `require_instrument` before any process
starts; the halt probes loop over `${instruments[@]}`.

## Acceptance criteria

1. **Free ports by default.** Two runs started together
   (`scripts/e2e-kill-switch.sh & scripts/e2e-kill-switch.sh; wait`) both
   print `E2E KILL-SWITCH PASSED`. Before the change one of them fails.
2. **Taken ports are named, early.** With a listener on the sentinel port
   (`SENTINEL_PORT=<p>` and a `python3` listener on `<p>`) the script exits 1
   within 2 s, prints `port <p> (risk-sentinel) is already in use`, starts no
   process (`pgrep` finds no `risk-sentinel`, `exchange-core` or `loadgen`
   afterwards) and does not print the dump of the logs (there are none).
   Same for `CORE_PORT` with `(exchange-core)`.
3. **Overrides still work.** `CORE_PORT=<free> SENTINEL_PORT=<free>
   scripts/e2e-kill-switch.sh` passes and uses exactly those ports (the
   `==> starting ... on :<port>` lines show them).
4. **Single instrument list.** `grep -n 'instruments=\|--instruments' 
   scripts/e2e-kill-switch.sh` finds the list declared once; the exchange
   and replay command lines are built from it. Changing it to `(1 2 3 4)` makes
   the script exit 1 within 2 s with `instrument 5 (sweep) is not in
   --instruments`, before any process starts.
5. **The header explains the choice** of 1-5 and says `config/exchange.json`
   is deliberately not used (it lists 1-4); the README line about the script is
   unchanged unless it said otherwise.
6. **No regression, also under load.** The script passes 3 consecutive runs
   idle and 3 consecutive runs with 32 busy-loop processes on the 8-core host
   (as measured in Context item 3); `scripts/e2e-smoke.sh` (plain and
   `--config`) still passes. A flake seen in these runs is investigated, not
   retried away.
7. `shellcheck` (if installed) and `bash -n` pass; `scripts/docs-check.sh`
   passes. The repository-wide definition of done in CLAUDE.md holds; the
   C++ and Rust code is unchanged, so the ctest and cargo runs are expected
   to be unaffected, and CI runs them.

## Files expected to change

- `scripts/e2e-kill-switch.sh`
- `docs/tasks/022-harden-e2e-kill-switch.md`, `docs/tasks/README.md`,
  `ROADMAP.md`, `mkdocs.yml`

## Out of scope

- The same change to `scripts/e2e-smoke.sh`.
- Changing `config/exchange.json` or the compose files (adding instrument 5
  to the shipped configuration for the benefit of a test).
- Retry logic in the script around loadgen (loadgen already retries
  back-pressure) or longer timeouts (no flake was observed).
- Closing the small window between choosing a free port and the process
  binding it; the script fails closed if it loses that race.

## Dependencies

- **Hard:** 019 (the script), 021 (its sweep step and instrument 5).
