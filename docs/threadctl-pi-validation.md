# Initial ThreadCtl Pi validation results

This records the initial revision, before the manager-stall fix. The current
independent enforcement path passes the 20 ms confirmation check in 30 Pi
manager-stall trials; see [the manager-stall fix results](manager-stall-fix-validation.md)
and [current checklist](threadctl-checklist.md). Earlier failures below remain
historical evidence and are not the status of the corrected build.

Validated on 21 November 2026 against target `10.78.56.80` through QNX qconn and
the documented QNX GDB upload/launch/download interface. The target reports
QNX 8.0.0, RaspberryPi4B, aarch64le and four CPUs. Tests run as root; the
production user's abilities remain to be qualified.

Executables were staged under `/tmp/pg-threadctl-73146`. Every scenario run
selected a distinct `PG_INSTANCE`, and cleanup stopped its own processes.
Post-test process inspection found no remaining staged PowerGuard/runtime
processes. Installed application binaries were not replaced.

## Configuration

- Release build, QNX SDP 8 AArch64, `-O2`, extra warnings treated as errors and
  GCC compiler analysis. Debug build also completes with warnings treated as errors.
- CPU0: supervisor priority 25, watchdog 26, coordinator 22, manager 20.
- Critical process: CPU1, FIFO 30, 100,000 fixed arithmetic iterations,
  10 ms period and 5 ms deadline.
- NC1: CPU2, FIFO 10. NC2: CPU3, FIFO 8. Five-millisecond worker periods,
  immediate gate-change ACKs and 50 ms absolute leases.
- CPU utilization: real idle-thread counters over a 100 ms sliding window,
  refreshed every 10 ms. Idle TIDs 1/2/3/4 were checked against CPU mapping,
  priority and names `idle_cpu_0` through `idle_cpu_3`.
- Scenario temperature: explicitly SIMULATED. Standalone endpoint-read test:
  REAL, Celsius, quiet mode, no deliberate heating.

## ThreadCtl API assertions

`pg-threadctl-check` passed on the Pi, including 10,000 observed CPU-number checks
on each of the four CPUs, current/inherit mask readback, 64-bit exchange and
restoration, invalid-mask rejection, child-thread inheritance, spawned-process
inheritance, named FIFO scheduling and restoration of the original masks.

The diagnostic also passed hold/continue of a disposable lock-free thread,
thread-exit pulse registration, and cancellation of that notification. Production
workload restrictions do not force-hold threads.

## Functional scenarios and metrics

| Capture | Cases | Archived critical jobs | Critical misses / gaps | Restriction result |
| --- | --- | ---: | --- | --- |
| `nominal` | Two spikes, full ramp, stale temperature, staged recoveries | 5,930 | 0 / 0 | 5 confirmed, 0 late, worst 11.725 ms |
| `repeat30` | Thirty spike/recovery cycles and matching Normal-to-Warning trials | 13,690 | 0 / 0 | 30 confirmed, 0 late, worst 9.691 ms |
| `faults` | One-second manager pause and one-second watchdog pause, then restoration | 1,619 | 0 / 0 | Worker closure assertions pass; supervisor-visible restriction timing fails, worst 967.984 ms |

Across the two final nominal captures, all 35 scored restrictions met the
20 ms target. Maximum archived critical response was 1.136037 ms. In the
30-trial capture critical response p99 was 0.688093 ms and maximum was
1.119852 ms. Its Normal-to-Warning transition-delay spread was 5.099 ms
over 30 matching trials. That spread has been measured, but an acceptable
jitter bound has not yet been declared.

Worst full restoration across the two nominal captures was 3,042.589 ms,
including the required three-second continuous dwell. CPU overshoot was at
most 6 percentage points and estimated-demand-index overshoot was 2 points
in the implemented 100 ms scoring windows. Simulated ramp overshoot was
0.0 C in that same scoring window; this is not a real thermal-peak result.

The quiet onboard reader acquired all 120 planned samples at 25 ms intervals
over 3 s, with zero missing samples, read errors, overflows or invalid CPU
windows. Main-thread CPU0 placement was verified before creating its threads.
The receiver and sampler used 3,797,537 CPU ns in total, approximately 0.127%
of one core for this short capture.

## Findings retained from diagnostic runs

- The image reports idle base priority 1. Startup now qualifies thread identity
  and CPU mapping instead of assuming idle priority 0.
- Deferring cap ACKs to the next worker period could reach the supervisor's
  20 ms timeout during ordered restoration. ACKs now occur immediately at the
  completed-work boundary.
- A short worker reporting timeout exposed a conservative closed-state fault.
  The coordinator now runs at base priority 22 and noncritical reporting has a
  bounded 5 ms wait. Critical reporting remains bounded at 2 ms.
- CPU-window duration is now explicitly 100 ms, refreshed at 10 ms, rather than
  conflating a short desktop burst with sustained utilization. Injection-to-limit
  detection includes that window; 20 ms is not claimed from load injection.
- This revision's service-stall confirmation delay exceeded 20 ms. The later
  independent-enforcer fix closes that item; its results are linked above.

## Evidence locations

Local raw files are under `results/threadctl-pi-validation/`: `nominal/`,
`repeat30/`, `faults/`, `thermal/`, compiler logs and diagnostic transcripts.
Each scenario directory contains raw jobs and samples, readiness/process
reports, assertions, scorecards and `critical-analysis.json`.

Corresponding target directories are `threadctl-results-12484664`,
`threadctl-results-14065727` and `threadctl-results-19492928` beneath the staging
directory. Scorecard and recorder counts differ by at most one job because
they stop at different instants; each archive's recorded interval is contiguous.

See [the checklist](threadctl-checklist.md) for remaining real thermal integration,
LED, crash/hang, permission, soak, overhead, comparison and Pi 5 qualification.
