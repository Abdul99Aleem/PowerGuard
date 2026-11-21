# PowerGuard implementation and Pi testing checklist

Updated 21 November 2026. Current integration merges teammate-3 `5c37cb5`
into the ThreadCtl branch based on `b62852e`. Implementation, archived teammate
results, and current-build Pi evidence are kept separate.

The target is **10.78.56.80**, QNX 8.0.0, four-core Raspberry Pi 4B. Tests use
private `PG_INSTANCE` names and staged files under `/tmp/pg-t3-merge-2111`.
See [the integration and validation report](teammate3-merge-validation.md) and
[the evidence summary](validation/teammate3-merge-pi4-summary.json).

## Completed integration and fresh Pi verification

| Requirement | Implementation | Pi validation |
| --- | --- | --- |
| ThreadCtl runmask plus inheritance | Complete; original placement preserved | CPU0/1/2/3 readback and child-thread/process inheritance pass |
| 64-bit query/exchange, restoration and invalid masks | Complete | All target assertions pass |
| Thread naming, FIFO scheduling and strict readiness | Complete; no ordinary-scheduling fallback in split runtime | Target qualification passes; negative non-root test remains |
| Isolated hold/continue and exit-event diagnostics | Diagnostic only; production uses cooperative gates | Target assertions pass |
| Separate critical, NC1 and NC2 processes | Complete | Unique PIDs; critical CPU1/FIFO30, NC1 CPU2/FIFO10, NC2 CPU3/FIFO8 |
| Independent enforcer | Retained; sole authorized split-runtime grant sender | Remains alive through manager and supervisor stalls |
| Manager progress and immutable receipt | Retained; progress timeout remains 50 ms | 30/30 one-second manager stalls confirm before resume, worst 12.135167 ms from expiry |
| Complete critical-job delivery | Retained full ring and batch protocol; original source IDs | 21601 raw jobs, zero gaps/misses; max 1.148518 ms against 5 ms |
| Restarted manager cursor and heartbeat sequence | Fixed; resume supervisor-accepted job ID and sequence | Ten manager restarts with zero missing job records |
| CPU input and utilization window | Measured per-core idle clocks, 100 ms sliding window refreshed every 10 ms | Idle identity/mapping and valid windows pass |
| Temperature freshness and simulated judging scenarios | Existing 150 ms expiry preserved | Ramp, real CPU spike, freeze and staged recovery pass |
| NC2-first restriction and NC1-first restoration | Original split-runtime ordering retained | Current target assertions pass; raw effects retained |
| Nonblocking supervisor progress observation | New lock-free shared mapping; publish at least every 100 ms, check every 50 ms | Supervisor SIGSTOP test passes while receive loop is stopped |
| Persistent watchdog forced-off | New immutable latch at 200 ms lost progress; ordinary grants cannot clear it | Observed off/latch by 235.188630 ms from last progress; remains off after resume |
| Legacy manual admission after unqualified startup | Reject positive commands and enforce the latch at every worker boundary | Cold-start test without a policy epoch passes |
| Watchdog heartbeat loss | Existing independent lease/readiness protection retained | One-second watchdog stall closes workers and recovers after resumption |
| Legacy bounded compute and cap wake-ups | Teammate's 500 us chunks and monotonic condition wake-ups retained | Handoff, B/C/A smoke and legacy watchdog-fault tests pass |
| Benchmark failure propagation | Fixed: missed jobs and B/C budget violations fail the suite | Negative host/target qualification tests and positive Pi smoke tests pass |
| Benchmark build integration | Fixed includes, feature definitions, dependencies and asset ordering | Root debug/release, default/handoff/overhead/HAM-suite builds pass |
| Operator CLI and labelled source | Existing status retained; FORCED_OFF explicitly reported | Current target output and CLI assertions pass |
| Interactive jury menu and command runbook | Complete; presentation wrapper around the validated binaries | All menu actions exercised on Pi, including fresh-run reset and clean exit; manager receipt 7.206833 ms, watchdog latch 225.451574 ms. See [runbook](jury-runbook.md) |

Affinity limits where our workers execute. OS work, interrupts and shared cache
contention remain possible on those CPUs. It is not a CPU reservation or power
measurement.

## Five scored metrics

| Metric | Current evidence | Still pending |
| --- | --- | --- |
| Detection to effective restriction and receipt | Pre-B5: 36 confirmed, zero late/unconfirmed; worst effect 13.820500 ms and receipt 14.798204 ms. Manager-stall subset: 30/30, worst receipt 12.135167 ms | Broader matched-load/interruption and failure-path characterization |
| Critical deadline misses | 21601 archived current-build jobs; zero gaps, invalid timestamps, period errors or misses; maximum 1.148518 ms | Long soak, real control algorithm, deliberate skipped-release failure and adversarial IRQ/system load |
| Thermal/workload overshoot | Existing 100 ms score windows: simulated thermal 0.0 C, CPU 6 percentage points, estimated demand 2 points | Real thermal peak/cooldown through 5 s after restriction; accepted bounds |
| State-transition jitter | Normal-to-Warning spread 2.051 ms, n=2 | Full repeated matched-phase series against the PRD bound; other transitions |
| Controlled restoration | Worst 3057.061 ms across 35 recoveries, including 2 s + 1 s observation | More recovery cycles and boundary-phase fault tests |

The 20 ms receipt origin is a qualifying sampled threshold or an input/progress
expiry. The manager timeout is still 50 ms; worst manager pause-to-receipt is
61.314777 ms. CPU injection also includes the declared utilization window.

Stopping the supervisor itself is a separate B5 fault. Its receive loop cannot
accept an effect while stopped. The combined post-B5 score retains
1 late restriction, worst 959.400 ms. This is not hidden or
substituted for the pre-B5 result. B5 passes its independent 270 ms gate/latch
oracle and preserves complete critical jobs. A full new run is required to
restore capacity after this watchdog trip.

## Overhead and historical evidence

The fresh legacy smoke comparison uses one approximately 2 s window per mode:
B 0.47866%, C 0.45279%, A 99.91499% of one core. C sends 75% fewer notices than B.
All three windows have zero critical misses/gaps. These are short regression
measurements, not full overhead qualification or a C-versus-B CPU saving claim.
They exclude a real sensor and do not include the split runtime's enforcer.

The imported teammate archive contains a nine-window quiet replay comparison
and reports 54,000 releases with zero misses; its complete per-job overhead
traces are not bundled. Its final raw handoff file has 1,334 records and a
1.877203 ms maximum, correcting the DOCX's 1.740 ms. See the integration report
for provenance and the initial failed cross-runtime probe retained during this
integration.

The earlier ThreadCtl validation and 30 manager-stall results at `b62852e`
remain historical baselines in [the previous stall report](manager-stall-fix-validation.md).
The quiet real-thermal characterization previously acquired all 120 planned
reads over 3 s without errors; that validates the reader, not an integrated
real-temperature control loop or total-stack overhead.

## Remaining delivery checks

- [ ] **PG-PERF-01 — trace-on restriction margin:** attribute the 20.646278 ms
  full-trace result and validate a larger margin below the unchanged 20 ms
  target. The trace-off physical demo records 18.729 ms; the diagnostic run
  preserves zero misses/gaps across 5,161 critical jobs. See the
  [measured breakdown and optimization plan](timing-investigation.md).

Real-sensor follow-up on 21 November 2026: the new runtime sink passes physical
input, managed CPU spike, producer pause and staged recovery on the Pi 4B.
730 real readings and 2,135 complete critical jobs were captured, with zero
misses/gaps. See the [live report](real-thermal-integration.md). Physical
conversion freshness and threshold peak/cooldown remain separate open items.

- [ ] Repeat the full ABC/BCA/CAB overhead series with corrected failure gates and archive every job trace.
- [ ] Measure total split-stack monitoring/management overhead, including the enforcer and actual thermal adapter, against the 2% budget.
- [x] Integrate one real thermal producer; verify read age, producer-stop fail-safe and staged recovery on Pi 4B.
- [ ] Qualify physical conversion freshness and frozen-conversion handling beyond the endpoint's READ_ONLY format.
- [ ] Capture physical thermal peak/cooldown and set acceptable overshoot bounds.
- [ ] Repeat the matched-phase transition series and qualify the PRD jitter bound.
- [ ] Repeat recovery interruption at 1.99 s and immediately after NC1 resumes.
- [ ] Validate physical LEDs, wiring, and all five states against CLI output.
- [ ] Exercise worker death, coordinator crash/hang, enforcer crash/hang and complete restart/requalification.
- [ ] Deny required affinity/scheduling/high-resolution abilities under the deployment account and verify loads remain disarmed.
- [ ] Run representative long-duration critical-load and IRQ/system-interference soak.
- [ ] Deliberately overrun acquisition/control releases and require the test to fail.
- [ ] Run a matched affinity comparison before claiming an improvement caused by placement.
- [ ] Qualify Pi 5/BSP separately.
- [ ] Optional: activate HAM and prove automatic restarts in a separately scoped trial; only HAM-suite compilation was checked here.

Hardware power metering and dynamic frequency control remain optional stretch goals.
