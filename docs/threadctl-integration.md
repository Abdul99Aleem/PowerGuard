# PowerGuard ThreadCtl integration

The [architecture and problem-resolution guide](powerguard-design-and-manager-stall-fix.md)
explains the original failures and the complete corrective design. This page
focuses on integration and operation.

The latest [teammate-3 integration report](teammate3-merge-validation.md) covers
restart cursors, nonblocking supervisor progress, the persistent watchdog latch,
benchmark corrections and fresh Pi evidence.

This implements the affinity and readiness design from the ThreadCtl PRD on
QNX SDP 8. Use the new `pg-runtime` stack for this integration. The original
`pgbench serve` remains an explicitly legacy threaded testbench; rebuilding it
does not turn that legacy mode into the separate-process runtime.

## QNX interfaces and their purpose

The implementation follows the current
[ThreadCtl reference](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/threadctl.html)
and [processor affinity guidance](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/multicore_processor_affinity.html).

| Interface | Integrated use |
| --- | --- |
| `_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT` | Set and read back runmask plus child inheritance; allocate the documented word count for the actual CPU topology |
| `_NTO_TCTL_RUNMASK_GET_AND_SET_64` | Independently verify the calling thread's current mask using a uint64_t; no mask change when queried with zero |
| `_NTO_TCTL_NAME` | Set and verify application thread names |
| `ThreadCtlExt(1, tid, _NTO_TCTL_NAME, ...)` | Read the procnto idle-thread names when qualifying per-core CPU counters; does not change another process's affinity |
| `pthread_setschedparam` and `pthread_getschedparam` | Require and verify FIFO policy and declared priority; no ordinary-scheduling fallback |
| QNX messages with `TimerTimeout` | Worker reports, grant delivery, effective acknowledgements and bounded reporting |
| High-resolution monotonic timer pulses | Coordinator CPU sampling and health checks |

Critical/noncritical absolute releases and the manager's waits also request
1 ns timer tolerance. This is a tolerance request, not a 1 ns scheduling claim.

The `pg-threadctl-check` executable additionally exercises direct `_NTO_TCTL_RUNMASK`,
64-bit exchange/restoration, invalid masks, child-thread and spawned-process
inheritance, local `_NTO_TCTL_ONE_THREAD_HOLD/CONT`, and `_NTO_TCTL_ADD_EXIT_EVENT/DEL_EXIT_EVENT`.
Hold/continue is tested only on a disposable thread that owns no application
locks. Production caps use cooperative execution gates, not forced thread holds.
No I/O privilege or alignment command is requested: it does not contribute to
this workload-management requirement.

Affinity determines eligible CPUs. It is not a CPU reservation, frequency
controller or thermal guarantee. Existing OS activity and interrupts can still
execute on the chosen CPUs and shared memory/cache contention remains possible.

## Process placement

| Process | CPU | Base FIFO priority |
| --- | ---: | ---: |
| Supervisor | 0 | 25 in the documented launch |
| Workload manager | 0 | 20 |
| Independent enforcer and effect reporter | 0 | 24 |
| Runtime coordinator | 0 | 22 |
| Independent watchdog | 0 | 26 |
| Critical-control worker | 1 | 30 |
| NC1 worker | 2 | 10 |
| NC2 worker | 3 | 8 |
| Thermal characterization monitor | 0 | 18 |

The coordinator launches three separate worker processes. Each configures and
verifies its own masks, name and scheduling before reporting READY. Workers
recheck their placement on every report. Configuration failure exits visibly;
placement drift or a lost report withdraws noncritical permissions.

The existing thermal monitor also sets its main thread and inheritance mask to
CPU0 before creating receiver/heater threads. Its optional characterization
heater retains its lower scheduling priority. This does not yet make it the
sole telemetry producer in the separate-process scenario stack.

The coordinator does not enable runnable noncritical work until all three
workers are reporting, an independent watchdog heartbeat is current, and an
independent enforcer is current and an unexpired supervisor grant exists. A worker enforces its own absolute
grant expiry even if the coordinator or manager stalls. Worker death or report
loss latches a runtime fault: restart and requalify the isolated stack to clear it.
These are cooperative worker safeguards, not containment for malicious workers.

NC2 acknowledges closure before NC1 is told to close for a normal two-worker
restriction. NC1 acknowledges restoration before NC2 receives a positive grant.
Emergency independent lease expiry closes each worker locally; a stalled
worker must not be credited with an acknowledgement it has not supplied.
Workers report a changed gate immediately at the completed-work boundary;
they do not defer its acknowledgement to the next worker period.

`pg-manager` supplies telemetry, critical-job records and a sequenced progress
heartbeat. `pg-enforcer` alone forwards decisions and reports matching worker
effects. The supervisor times manager progress independently at 50 ms; effect
heartbeats cannot keep a stalled manager healthy. On progress expiry it commands
Critical, and the enforcer can confirm both worker gates while the manager
remains suspended. Recovery still requires fresh manager progress, telemetry and
the original dwell. A restarted manager now resumes the supervisor's accepted
job cursor and progress sequence. Missing or overwritten runtime records still
fail qualification; release IDs are never silently renumbered.

The workload owner creates an instance-specific progress mapping before the
supervisor starts. The supervisor publishes lock-free progress at least every
100 ms. The watchdog checks it every 50 ms without sending a blocking policy
request. At 200 ms lost progress it latches forced-off. The 270 ms B5 check
verifies worker closure and persistent off after supervisor resume. Restart the
complete run to clear this latch; fresh telemetry or a manager restart cannot
clear it. The existing 100 ms watchdog-heartbeat readiness check remains separate.

`PG_EFFECT_CONFIRMED` and `PG_CONFIRM_READ` return the supervisor's immutable
first accepted receipt timestamp and qualifying detection timestamp. The fault
test checks their difference against 20 ms before sending SIGCONT. That bound
begins at the unchanged 50 ms manager-progress expiry, not at SIGSTOP itself.

## CPU measurements and critical work

The runtime accepts four idle-thread IDs in CPU order. It checks each identity
through procfs, its `idle_cpu_N` name, idle base priority and CPU mapping before
using `ClockId(1, tid)`. On the tested image these are TIDs 1, 2, 3 and 4, with
base priority 1. Do not reuse that mapping on another image without inspection.

Per-core utilization is calculated from each idle thread's accumulated CPU
time over a declared approximately 100 ms sliding window, refreshed every 10 ms.
The first 100 ms is not a valid CPU window. Invalid counter
windows are explicitly marked invalid. The manager uses this four-value array;
it no longer duplicates aggregate process CPU across the four cores in the new
runtime. Temperature scenarios remain labelled SIMULATED. Onboard thermal
qualification is a separate remaining requirement.

The 20 ms restriction target starts at the qualifying sampled threshold or
freshness-expiry timestamp. A CPU spike's injection-to-restriction time also
includes the declared utilization-window detection delay; it is not asserted
to be 20 ms from injection.

Sustained unrelated work can cross a CPU limit and legitimately restart recovery.
The 2 s plus 1 s clear periods are minimum continuous observations, not a promise
that every recovery finishes exactly three seconds after a scenario command.

Critical control performs a fixed number of volatile arithmetic iterations
(default 100,000) every 10 ms, with a 5 ms deadline. Record the iteration count
and build profile with results. This is a synthetic critical workload, not
calibration of the eventual application control algorithm.

Every reported critical job includes its source ID, release, start, absolute
deadline and finish. A bounded coordinator ring retains 2,048 records. The
manager drains batches of 16 and fails qualification on a gap/overwrite rather
than silently publishing only the latest job. The streaming recorder must run
for captures longer than the ring retention interval. A lost critical IPC
report is counted and invalidates a clean qualification result.

## Build and start

Build root `make all` in the QNX SDP 8 environment, preferably with
`BUILD_PROFILE=release` for timing runs. Also build `testbench/` for its updated
`pgbench` operator client. Bench protocol is now version 4; rebuild clients,
runtime, enforcer and manager together. Existing supervisor v1 request/reply
layouts are unchanged; progress and confirmation messages are additive extensions.

Place these files together on the Pi:

```text
PowerGuard  pg-manager  pg-enforcer  pg-runtime  pg-console
pg-check  pg-manager-check  pg-merge-check  pg-threadctl-check  pg-validate
pg-stall-check  pg-supervisor-check  pgbench
validate-threadctl.sh
```

Inspect the target first:

```sh
uname -a
pidin syspage=cluster
pidin -p 1 threads
```

For the verified four-core image, a manual isolated launch is:

```sh
export PG_INSTANCE=threadctl_demo
./pg-runtime serve 1 2 3 4 100000 &
sleep 1
./pg-runtime watchdog &
./PowerGuard 25 &
sleep 1
./pg-enforcer &
./pg-manager &
```

Set the same `PG_INSTANCE` in every operator terminal. Without that variable,
the default application names are used. Separate instance names avoid replacing
or connecting to an installed demo; they do not isolate physical CPU resources.
Do not run legacy `pgbench serve` under the same instance name.

Read the consolidated status and record raw evidence:

```sh
./pg-runtime status
./pg-runtime record > jobs.csv &
./pg-runtime samples > samples.csv &
./pg-console --live scores
```

`status` includes process/thread placement, effective caps, CPU values, critical
statistics, policy causes, sensor age/source/quality and clear-dwell progress.
The recorder and sampler must be stopped before reading their final output.

## Pi validation and showcase

Run the supplied test script beside the staged binaries:

```sh
sh validate-threadctl.sh 1 2 3 4
```

It selects its own service-name instance, captures platform metadata, runs the
ThreadCtl and policy assertions, starts only its own processes, and verifies
Normal operation, repeated spike restrictions, the temperature ramp, stale
temperature fail-safe, recovery, manager-stall grant expiry and watchdog-stall
grant withdrawal. Cleanup targets only the PIDs started by the script.

The default two spike trials are a smoke test. `PG_TRIALS=30` increases repeated
Normal-to-Warning transition trials. Use matched baseline conditions and retain
cancelled trials. The scorecard's state-transition jitter is a trigger-to-state
delay spread, not critical-task release jitter; both are recorded separately.

For separate performance and fault evidence, run:

```sh
PG_TRIALS=30 PG_FULL_SCENARIOS=0 PG_FAULT_TESTS=0 sh validate-threadctl.sh 1 2 3 4
PG_TRIALS=0 PG_FULL_SCENARIOS=0 PG_FAULT_TESTS=1 sh validate-threadctl.sh 1 2 3 4
PG_TRIALS=0 PG_FULL_SCENARIOS=0 PG_STALL_TRIALS=30 PG_FAULT_TESTS=1 sh validate-threadctl.sh 1 2 3 4
```

The manager-stall test holds SIGSTOP for one second and requires the supervisor
receipt within 20 ms of progress expiry, before resuming the manager. Audit the
raw timestamp origin and bound with
`python tests/analyze_manager_stalls.py CAPTURE/stall-confirmations.txt`.
Retain previous failed captures as historical evidence; they do not describe
the corrected independent confirmation path.

After copying a completed capture to the host:

```sh
python tests/analyze_threadctl.py PATH_TO_CAPTURE/jobs.csv
```

The analyzer checks contiguous records, timestamp order, the 10 ms release
schedule and every 5 ms deadline, then reports p50/p95/p99/max response,
release lateness and execution wall time. It cannot establish response-time
guarantees from a finite run or fill missing records.

Use LEDs and the CLI alongside the scenarios for judging. Physical LEDs, real
thermal peak/overshoot, long-duration deadline evidence and matched affinity
comparisons require their own acceptance runs. See `threadctl-checklist.md`
for the current measured status.
