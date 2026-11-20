# PowerGuard

**Deterministic Power and Thermal-Budget Management for QNX Edge Systems**  
**Team Schedulix · QNX eHACK National Finals · QNXHACK-HYD-014**

> **Project status:** This branch implements K’s QNX supervisor, five-state policy, console and metric checks. The live scorecard needs worker acknowledgements and critical-job telemetry from the integrated Pi.

## Project goal

PowerGuard is a mixed-criticality resource supervisor for **one Raspberry Pi 4 or 5 running QNX**. It protects one periodic critical-control workload while managing two lower-criticality workloads (NC1 and NC2) in response to CPU, temperature, estimated-power and sensor-integrity conditions.

Core invariants:
- Never intentionally throttle, suspend, or demote critical control.
- Restrict **NC2 before NC1**; restore **NC1 before NC2**.
- Use explicit **Normal → Warning → Throttled → Critical → Recovery** policy states, separate activation/recovery thresholds, continuous dwell and staged restoration.
- Missing or invalid temperature sensing triggers conservative restrictions.
- Record **actual effective enforcement** separately from the moment a command is issued.
- Avoid unbounded synchronous waits in the supervisor, and require an independent worker/manager lease fail-safe.
- Label telemetry **REAL** or **SIMULATED**; measured timing is evidence, not an unconditional hardware guarantee.

## Team responsibilities

| Member | Workstream | Assigned cards |
| --- | --- | --- |
| **A** | CPU workload engine, affinity, profiles, stress, manager IPC, actual restriction, leases, watchdog and GPIO LEDs | PG-05–08, PG-15–16, PG-28, PG-26 |
| **K** | Supervisor policy, thermal quality, estimated power, hysteresis/recovery, CLI and metrics contract | PG-14, PG-18–23, PG-29, PG-25 |
| **R** | QNX platform, build/deploy, CPU accounting, protected critical task, tracing, scenario integration and demo evidence | PG-01–04, PG-09–13, PG-17, PG-24, PG-27 |

**Execution board:** [PowerGuard QNX execution](https://trello.com/b/RcyliKfX/powerguard-qnx-execution). Use the PRD and the relevant PG-XX card for acceptance criteria.

## Requirements and demonstrations

The target must run the critical task and NC1/NC2 plus temperature monitor, supervisor, workload manager and watchdog **on the same QNX Pi**. The required operator interfaces are a read-only CLI and five physical state LEDs.

Judging scenarios:
1. Gradual temperature rise and staged restrictions.
2. Sudden noncritical CPU spike and real budget enforcement.
3. Stale/frozen temperature input and conservative fail-safe behavior.
4. Controlled, hysteretic, staged recovery.

Five scored metrics: detection-to-effective-restriction latency, critical deadline misses, thermal/workload overshoot, state-transition jitter, and restoration time. Archive reproducible QNX trace/raw timestamps, configurations, board/BSP identity, sample counts and measured maxima.

## Repository and Momentics setup

**Keep portable IDE metadata in Git**: `.project`, `.cproject`, applicable `.settings/` preferences, source, headers, and the actual project Makefiles. **Do not commit** compiled binaries, generated build output, Eclipse workspace `.metadata/`, private credentials, or captured result bundles.

### Build and try the supervisor

Import the cloned repository using **File → Import → General → Existing Projects into Workspace** in Momentics 8. Select **PowerGuard**, choose **aarch64le-debug**, and run **Project → Build Project**. From a QNX SDP 8 environment, `make all` builds the same three programs in `build/aarch64le-debug/`.

| Program | How to run it |
|---|---|
| `pg-console` | For a first run, choose this binary in a C/C++ QNX Application launch configuration on your Pi. Enter `55 86 30` as the arguments (temperature °C, busiest-core CPU %, power index). The simulated policy reports **Throttled**, NC1 100%, NC2 0%. Leave arguments empty for interactive input or use `--example` for a scored scenario. |
| `pg-check` | Run without arguments to exercise the policy transitions, freshness, acknowledgements and five scoring calculations. |
| `PowerGuard` | Run the live service with your configured FIFO priority as its argument (e.g. `20`). Select a priority below the critical task and above the background workers. |

In a local console session, try `55 30 30 frozen`, then `55 30 30 good 3100` to see progressive recovery. In live mode the supervisor starts conservatively. A producer supplies fresh measurements and the workload manager acknowledges effective caps. Use `pg-console --live scores` to read the scorecard. The scenario driver can also supply labelled simulated input, e.g. `pg-console --live 55 86 30 good 1000` in place of the regular producer for that run.

The [IPC header](include/ipc.h) defines the local versioned contract. Use `PG_SAMPLE` for a derived power estimate or `PG_SAMPLE_POWER` for an explicit index. The manager reads `PG_READ` and confirms both applied budgets with `PG_EFFECT`. The timing reporter sends one `PG_JOB` per release. The scenario driver arms `PG_TRIAL` for comparable repeated transitions. `PG_SCORE` returns restriction latency, critical misses, overshoot, jitter and restoration time. All timestamps are in the target’s monotonic nanoseconds. The 20 ms reaction target is provisional until measured on the Pi.

The portable tests can also run on a development host with a C11 compiler:

```sh
cc -std=c11 -Wall -Wextra -Werror -Iinclude supervisor/policy.c supervisor/metrics.c tests/replay.c tests/metric_checks.c -o pg-check
./pg-check
```

### Working on an assigned card

```powershell
git switch main
git pull --ff-only origin main
git switch -c feature/PG-05-cpu-worker   # example for A
# edit, build and test
git add -A
git status
git commit -m "feat: implement PG-05 CPU workload engine"
git push -u origin feature/PG-05-cpu-worker
```

Create a pull request to `main` and request review from a teammate. One owner per module; review shared IPC header changes in advance. Do not mark cards done without target-specific acceptance evidence.

## Project layout

```text
PowerGuard/
├── README.md
├── .gitignore
├── src/PowerGuard.c             # QNX supervisor service
├── include/                     # supervisor policy, metrics and IPC headers
├── critical/                    # R
├── workloads/                   # A
├── manager/                     # A
├── watchdog/                    # A
├── supervisor/                  # policy and metric calculations (K)
├── thermal/                     # K
├── power/                       # K
├── telemetry/                   # R/K
├── cli/                         # K
├── gpio/                        # A
├── platform/qnx/                # R
├── tools/pg-console.c           # interactive test and IPC client
├── tests/                       # portable policy and scorecard checks
├── docs/
└── results/                     # generated runs (ignored)
```

Directories labeled planned do not necessarily exist yet.

## Hardware safety

Pi GPIO uses **3.3 V logic**; check sensor-breakout pull-ups, GPIO mapping, LED series resistors and button wiring before energizing. Hardware power metering and DVFS are optional. Keep the protected critical task independent of logging and CLI activity.

## References

- [QNX documentation](https://www.qnx.com/developers/docs/)
- [Team Trello board](https://trello.com/b/RcyliKfX/powerguard-qnx-execution)
- PowerGuard 3-member engineering PRD and handoff dossier (shared outside Git)
