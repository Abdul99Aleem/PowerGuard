# PowerGuard jury demo runbook

For the newly verified **real `/dev/thermal` path**, use the
[real-sensor acceptance demo](real-thermal-integration.md). The menu below
retains explicit simulated thermal ramp/freeze scenarios; its CPU load is real.

For a physical temperature trip at ordinary bench temperatures, use the
[lower-temperature executable](low-temperature-demo.md), clearly labelled with
its demo-only thresholds. For screenshots, use the
[System Profiler capture guide](system-profiler-screenshots.md).

One Pi, one terminal, one menu. The final submission uses the validated
ThreadCtl runtime and independent enforcer. This presentation wrapper does not
change the policy, thresholds or workload code.

## Open the demo on the prepared Pi

In a terminal connected to the prepared QNX Pi, using its current network address:

```sh
cd /tmp/pg-t3-merge-2111
sh jury-demo.sh 1 2 3 4
```

The four idle-thread IDs were qualified on this Pi 4 image. The menu starts
the complete stack and waits for confirmed Normal. It creates its own instance
and saves evidence under `jury-evidence-<pid>/run-<number>/`. Exit with **0** or
Ctrl+C; the menu stops only the processes it started.

## A simple three-minute presentation

Say: “PowerGuard protects a critical control task while reducing lower-priority
work when CPU load, temperature or data freshness exceeds our limits. QNX
ThreadCtl places the workloads on declared CPUs, and message passing joins
the policy, workers and independent enforcer.”

| Choose | Show the jury | What to say |
| --- | --- | --- |
| 1 | Normal state, temperature age, per-core CPU, caps, critical timing and misses | “The critical controller runs every 10 ms with a 5 ms deadline.” |
| 2 | A real NC2 CPU-load spike and the resulting restriction | “NC2 yields first. A more severe measured condition can stop both noncritical workloads.” |
| 5 | Controlled restoration | “Recovery requires continuous clear conditions: NC1 first, then NC2, with the 2 s plus 1 s observation.” |
| 4 | A frozen temperature producer; age rises and the system becomes conservative | “Repeated old data cannot refresh the original acquisition time.” |
| 5 | Fresh input and recovery | “One clear reading cannot immediately restore capacity.” |
| 6 | The manager pauses for one second, yet the enforcer confirms off before resume | “The limit is 20 ms from the declared 50 ms progress expiry, not from the pause command.” |
| 7 | Live scorecard and evidence location | “We retain worker effects and every captured critical job.” |
| 8 | Optional final watchdog demonstration | “Lost supervisor progress triggers a persistent off latch within a separate 270 ms limit.” |
| 9 | A fresh run after the watchdog latch | “Ordinary restore commands cannot clear the latch.” |
| 0 | Clean shutdown | “This completes our submission demonstration.” |

For the gradual thermal scenario, choose **3**. Allow about 32 s: Warning at
65 C, Throttled at 70 C, Critical at 75 C. Choose **5** afterward.

Temperature is **simulated** in these judging scenarios. Per-core CPU is
**measured on the Pi**. Power is a declared demand **index**, not measured watts.
Do not claim physical cooling, power savings or unverified LED behavior.

Show the passing scorecard with **7 before 8**. Deliberately pausing the
supervisor prevents it from promptly accepting an IPC receipt, so the late
receipt remains visible afterward. The watchdog proof uses its own independent
270 ms gate/latch result; choose **9** for a new normal run.

## Verified submission results

- 30/30 manager-stall confirmations; worst **12.135167 ms** from progress expiry.
- Ten manager restarts without missing job records.
- **21,601** captured critical jobs, zero misses/gaps; maximum **1.148518 ms**.
- Supervisor off/latch observed by **235.188630 ms**, within 270 ms, and remained
  off after resume.

These are the recorded regression results, not a promise that each live demo
will reproduce the same maximum. See the [evidence report](teammate3-merge-validation.md)
and [checklist](threadctl-checklist.md) for their scopes and remaining qualification.

The menu itself was exercised on this Pi on 21 November 2026: invalid input,
status, spike, ramp, freeze, recovery, manager stall, scorecard, watchdog latch,
new run and exit. The pre-watchdog scorecard showed five confirmed restrictions,
zero late/unconfirmed effects, and zero misses or missing records across 6,084
reported jobs. Manager confirmation took **7.206833 ms**; watchdog off/latch
observation took **225.451574 ms**. Restart returned to Normal and exit left no
PowerGuard demo processes running. This single presentation smoke run does not
extend the qualification scope above; its jitter card explicitly needed more
matching trials.

## Build and prepare another copy

In a QNX SDP 8 environment on the development computer, from the repository:

```sh
git switch main
git pull --ff-only origin main
make BUILD_PROFILE=release CCFLAGS='-Wextra -Werror'
make -C testbench
```

Copy these files to a new directory on the QNX Pi using Momentics Target File
System, the existing QNX development connection, or your configured transfer:

```text
build/aarch64le-release/PowerGuard
build/aarch64le-release/pg-runtime
build/aarch64le-release/pg-manager
build/aarch64le-release/pg-enforcer
build/aarch64le-release/pg-console
build/aarch64le-release/pg-validate
build/aarch64le-release/pg-stall-check
build/aarch64le-release/pg-supervisor-check
testbench/pgbench
scripts/jury-demo.sh
```

Place them together, then on the Pi:

```sh
cd /path/to/your/demo
chmod 700 PowerGuard pg-runtime pg-manager pg-enforcer pg-console pg-validate pg-stall-check pg-supervisor-check pgbench
sh jury-demo.sh 1 2 3 4
```

Keep `jury-demo.sh` in LF format. Use a terminal with the existing qualified
permissions; this submission was tested through the development connection as
root. No SSH key or new account is required for that connection.

If startup does not reach Normal, read the printed run directory's logs and
check the four idle-thread IDs, process readiness and actual background CPU
load. Do not lower thresholds to force a successful presentation.
