# Manager stall confirmation fix and Pi validation

For the complete problem statement, before/after architecture, implementation
walkthrough and operational troubleshooting, see
[PowerGuard architecture and manager stall fix](powerguard-design-and-manager-stall-fix.md).
The [portable aggregate summary](validation/manager-stall-pi4-summary.json)
is versioned with the source; raw capture bundles remain outside Git.

Validated on 21 November 2026 on `10.78.56.80`, QNX 8.0.0, Raspberry Pi 4B.
The staged release build is under `/tmp/pg-manager-stall-2111`; tests use separate
service-name instances. No installed application binary was replaced.

## Change

The telemetry manager previously forwarded both policy commands and worker
acknowledgements. Suspending that process delayed supervisor-visible
confirmation until the manager resumed, despite workers closing their gates.

`pg-enforcer` now owns the command/effect path independently, pinned to CPU0
at FIFO priority 24 with verified ThreadCtl masks. The manager retains telemetry
and complete critical-job delivery and sends monotonically sequenced progress
heartbeats. The supervisor separately expires manager progress at 50 ms and
commands Critical. Enforcer effect heartbeats cannot renew manager progress,
clear the fault or bypass recovery dwell.

The coordinator accepts grants only from its registered enforcer and requires
that enforcer's current heartbeat before enabling noncritical work. Bench
protocol is version 4; rebuild runtime, manager, enforcer and operator clients
together. Existing supervisor v1 message layouts are unchanged; progress and
confirmation commands are additive extensions.

## Measurement

`PG_EFFECT_CONFIRMED` stores the supervisor's first accepted receipt timestamp
for the current decision. Heartbeat repeats cannot rewrite it.
`PG_CONFIRM_READ` exposes that timestamp, the qualifying detection timestamp
and manager-progress timestamp to the test process.

`pg-stall-check` suspends the actual manager with SIGSTOP, requires a matching
Critical/both-off receipt, and verifies:

- detection timestamp equals last manager progress plus exactly 50 ms;
- worker effect is not earlier than detection;
- supervisor receipt is not earlier than the worker effect;
- supervisor receipt minus detection is at most 20 ms;
- the receipt arrives while the manager is still stopped.

The manager remains suspended for a full second after the measurement, then
resumes and must recover through the original clear observation and ordered
restoration. The timeout and response target were not increased. Worst observed
pause-to-receipt was 61.251740 ms: this includes the 50 ms detection timeout.
The 20 ms claim is detection-to-confirmation, not SIGSTOP-to-confirmation.

## Results

| Check | Result |
| --- | --- |
| One-second manager-stall trials | 30/30 PASS, 30 distinct decisions |
| Manager expiry to supervisor receipt | 6.477241 to 11.653796 ms; all below 20 ms |
| All fault/nominal restriction receipts | 37/37 below 20 ms; worst 12.856777 ms |
| Existing detection-to-effective-restriction score | PASS in both runs; 37 confirmed, 0 late/unconfirmed; worst 11.958 ms |
| Critical jobs during stall/watchdog run | 16,363 archived, zero misses, gaps or invalid timestamps; max response 1.179685 ms against 5 ms |
| Nominal spike/ramp/freeze regression | All state, cap, order and recovery assertions PASS; 6,031 archived jobs with zero misses/gaps |
| Worst restoration across both runs | 3,057.432 ms over 35 recoveries, including the original 2 s plus 1 s dwell |
| ThreadCtl and existing policy tests | PASS on Pi |
| New manager-progress/receipt policy checks | PASS on Pi: progress expiry, replay/wrong epoch/future-time rejection, effect heartbeats do not mask stall, first receipt immutable |
| Timestamp-audit negative tests | 4/4 PASS on host: a shifted origin, late receipt or replayed decision cannot produce a clean result |
| Builds | Debug and release PASS with extra warnings treated as errors; release compiler analysis PASS |

The fault-run receipts include 30 manager-progress faults, the watchdog fault,
and one additional restriction whose cause flags had cleared by receipt time.
Every recorded restriction is included in the 37-receipt audit; none were
removed to obtain the passing result.

Current nominal Normal-to-Warning jitter is 0.768 ms over two regression
trials. A new full nominal 30-trial series, its acceptance bound, and remaining
hardware/soak/failure-path work are tracked in the checklist.

## Reproduce and inspect

Place the rebuilt binaries and updated `validate-threadctl.sh` together on the
qualified Pi. The script also needs `pg-enforcer`, `pg-stall-check` and
`pg-manager-check` and starts/stops only its own processes.

```sh
PG_TRIALS=0 PG_FULL_SCENARIOS=0 PG_STALL_TRIALS=30 PG_FAULT_TESTS=1 sh validate-threadctl.sh 1 2 3 4
PG_TRIALS=2 PG_FULL_SCENARIOS=1 PG_FAULT_TESTS=0 sh validate-threadctl.sh 1 2 3 4
```

Verify idle-thread IDs before using those arguments on another image. On the
host, audit the downloaded capture:

```sh
python tests/analyze_manager_stalls.py CAPTURE/stall-confirmations.txt
python tests/analyze_threadctl.py CAPTURE/jobs.csv
python -m unittest discover -s tests -p test_manager_stall_analysis.py
```

Evidence is in `results/manager-stall-fix/repeat30/` and `nominal/`, including
raw jobs, samples, supervisor receipts, assertions and scorecards. The target
directories are `threadctl-results-21635128` and `threadctl-results-24981558`.
`confirmation-summary.json` audits all restriction receipts; the per-run JSON
files audit manager-stall timing and complete critical-job streams.

The nominal scorecard read contains 6,032 jobs while its recorder stops at
6,031; the stall-run counts both end at 16,363. This is an endpoint difference,
not a gap. Combined archives contain 22,394 jobs with zero deadline misses.

Prior failing captures under `results/threadctl-pi-validation/` are retained
for comparison. See [the current checklist](threadctl-checklist.md) for pending
enforcer/coordinator/worker fault tests, non-root abilities, physical LEDs,
real thermal-loop integration, longer interference/overhead tests and Pi 5.
