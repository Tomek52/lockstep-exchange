# 019: End-to-end kill-switch scenario

## Goal

Prove the whole risk loop across processes and languages:

1. a trader breaches a limit;
2. risk-sentinel blocks them;
3. exchange-core rejects their orders while others keep trading;
4. an aggregate loss engages the kill switch and halts everyone;
5. replaying the journals reproduces every rejection exactly.

## Context

- Loop semantics: [ADR-0013](../adr/0013-risk-feedback-loop.md). Determinism:
  [ADR-0004](../adr/0004-deterministic-replay-via-per-shard-journal.md).
- Pieces this relies on:
  - matching (002);
  - risk controls (004);
  - replay tool and digests (010);
  - risk client reports and acks (014);
  - sentinel session logic (016);
  - loadgen `breach` scenario (017).
- Existing smoke script to follow for structure: `scripts/e2e-smoke.sh`
  (process management, waiting on log lines, dumping logs on failure).
- CI job `e2e` in `.github/workflows/ci.yml`.

## Interfaces to implement

`scripts/e2e-kill-switch.sh`:

1. Start risk-sentinel with tight limits, for example
   `--max-abs-position 100 --max-trader-loss 1000 --kill-switch-loss 5000`.
2. Start exchange-core with `--journal-dir` in a temp dir and
   `--print-digest-on-exit`.
3. Run `loadgen --scenario breach`. Assert:
   - it exits 0 (the trader got blocked);
   - the sentinel log shows `BlockTrader` sent and `CommandApplied` received;
   - a concurrent `loadgen --scenario random --traders 1..5` run keeps
     getting accepts.
4. Drive a losing flow (a new loadgen scenario flag, or a second `breach`
   run with a loss-making pattern) until the sentinel engages the kill
   switch. Assert that subsequent orders from any trader are rejected with
   `TRADING_HALTED`.
5. Stop exchange-core. Run `lockstep-replay` on the journals and assert its
   digests equal the live `--print-digest-on-exit` digests.
6. Run `journal-dump` and assert it contains the `BlockTrader` and
   `KillSwitch` commands.

Add the script as a step of the CI `e2e` job, after the smoke test.

## Acceptance criteria

1. The script passes locally and in CI, and prints the scenario timeline:
   time to block, time to halt, number of rejected orders by reason.
2. Failure output includes both service logs (as the smoke script does).
3. The script is idempotent and cleans up processes and temp dirs on
   success and failure.
4. `README.md` gains a short "See the risk loop in action" section showing
   the command and an excerpt of its output.

## Files expected to change

- `scripts/e2e-kill-switch.sh` (new)
- `.github/workflows/ci.yml` (e2e job step)
- `rust/crates/lockstep-loadgen/src/scenario.rs` (loss-making flow, if needed)
- `README.md`

## Out of scope

- Unblock or resume flows (operator tooling).
- Docker-based variant (optional follow-up).

## Dependencies

- **Hard:** 002, 004, 010, 014, 016, 017.
