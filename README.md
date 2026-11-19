# PowerGuard

**Deterministic Power and Thermal-Budget Management for QNX Edge Systems**  
**Team Schedulix · QNX eHACK National Finals · QNXHACK-HYD-014**

> **Project status:** Repository bootstrap. The only available uploaded source is a Hello World C scaffold; no PowerGuard controller, sensor, scheduler or measured results are claimed complete. The **full QNX Momentics IDE project** (especially `.project`, `.cproject`, source and build rules) must still be imported from the local workstation.

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

The bootstrap source at `src/PowerGuard.c` is just the user-supplied Hello World example. The original local project is at:

```text
C:\Users\User\ide-8.0.3-workspace\PowerGuard
```

The connection to GitHub **cannot read this local Windows directory**. Before calling the repository a buildable Momentics project, compare/import the **real local project tree** and confirm `.project` and `.cproject` are present.

### For a teammate: clone and import

Install Git and the compatible QNX SDP/Momentics environment. In PowerShell:

```powershell
cd "$HOME"
git clone https://github.com/Abdul99Aleem/PowerGuard.git
cd PowerGuard
```

Once the real Momentics metadata has been pushed: **Momentics IDE → File → Import → General → Existing Projects into Workspace**, select this cloned folder, leave **Copy projects into workspace** unchecked, and verify the configured QNX toolchain, architecture and build variant before building. Until the metadata is committed, this import will not recreate the IDE project.

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

## Initial project layout (planned)

```text
PowerGuard/
├── README.md
├── .gitignore
├── src/PowerGuard.c             # Hello World placeholder only
├── include/                     # versioned IPC/configuration headers (planned)
├── critical/                    # R
├── workloads/                   # A
├── manager/                     # A
├── watchdog/                    # A
├── supervisor/                  # K
├── thermal/                     # K
├── power/                       # K
├── telemetry/                   # R/K
├── cli/                         # K
├── gpio/                        # A
├── platform/qnx/                # R
├── tests/                       # host, QNX and scenario checks
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
