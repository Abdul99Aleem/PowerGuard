# PowerGuard implementation and Pi testing checklist

Updated 21 November 2026. Implementation and target evidence are separate.
The tested target is **10.78.56.80**, QNX 8.0.0, four-core Raspberry Pi 4B.
Tests use the QNX development connection, staged files under
`/tmp/pg-manager-stall-2111` for the manager-stall fix, and a separate `PG_INSTANCE` for each run. The
installed application and its default service names are not replaced.

**Manager-stall confirmation is now closed:** 30 one-second SIGSTOP trials
confirmed at the supervisor before SIGCONT, worst **11.653796 ms** from the
unchanged 50 ms manager-progress deadline. All 37 restriction receipts across
the fault and nominal regressions were within 20 ms; worst **12.856777 ms**.
See [the manager-stall validation report](manager-stall-fix-validation.md).

## Completed implementation and tests

| Requirement | Implementation | Pi validation |
| --- | --- | --- |
| Set runmask and inheritance with ThreadCtl | Complete | Passed on CPUs 0, 1, 2 and 3 |
| 64-bit mask query, exchange and restoration | Complete | Passed |
| Runmask and inherit-mask readback | Complete | Passed |
| Child-thread inheritance | Complete | Parent CPU0 with child CPU1 passed |
| Spawned-process inheritance | Complete | Parent CPU0 with child CPU1 passed |
| Reject invalid CPU masks | Complete | Kernel rejection and unchanged masks passed |
| Thread names and scheduling readback | Complete | Named FIFO threads and declared priorities passed |
| Isolated hold/continue and exit-event diagnostics | Test coverage complete | All assertions passed; forced holds are not production throttling |
| Separate critical, NC1 and NC2 processes | Complete in pg-runtime | Distinct PIDs and CPU1/2/3 verified |
| Supervisor, coordinator, manager, enforcer and watchdog CPU0 placement | Complete | READY logs and target assertions passed; enforcer FIFO priority 24 |
| Thermal monitor CPU0 inheritance | Complete | Quiet onboard-reader run passed |
| Strict readiness and no ordinary-scheduling fallback | Complete | Successful configured startup verified; negative permission test remains |
| Noncritical gates, absolute leases and causal ACKs | Complete | Nominal restriction/recovery passed |
| Immediate acknowledgement at completed-work boundary | Complete | Nominal 20 ms restriction score passed after timing fix |
| NC2-first restriction and NC1-first restoration | Complete | Ramp/freeze restriction order checked; raw effect timestamps retained |
| Independent watchdog heartbeat | Complete | Manager and watchdog stall/restore assertions passed on corrected build |
| Confirmation independent of telemetry manager | Complete in pg-enforcer | 30/30 manager-stall confirmations received before manager resume |
| Separate manager-progress expiry and immutable supervisor receipt | Complete | Pi regression checks expiry, heartbeat replay rejection and first-receipt timing; 4 host audit tests pass |
| Genuine per-core CPU input | Complete in pg-runtime | Idle thread identity/mapping and counter validity passed |
| Declared CPU utilization window | 100 ms sliding window, refreshed every 10 ms | Valid windows recorded; injected-load characterization still needs broader runs |
| Fixed critical work and complete job reporting | Complete | 22,394 archived jobs across corrected fault and nominal runs; no gaps or misses |
| High-resolution releases and bounded IPC | Complete | Target timing recorded; permissions must be qualified for deployment account |
| Consolidated operator status | Complete in pg-runtime status | Target output verified |
| Raw job/sample capture and analysis | Complete | CSV archive and percentile analysis verified |
| Policy and scoring regression executable | Existing coverage retained | pg-check passed on target |
| Debug and release builds | Complete | Both cross-build; release also passes compiler analysis with warnings treated as errors |

CPU affinity excludes our workers from the critical worker's CPU. It does not
reserve the entire CPU against OS threads, interrupts or unrelated processes.

## Five scored metrics

Current captures are `results/manager-stall-fix/repeat30/` (30 manager stalls
and watchdog fault) and `results/manager-stall-fix/nominal/` (spike, ramp, freeze
and recovery). Target directories are `threadctl-results-21635128` and
`threadctl-results-24981558` under `/tmp/pg-manager-stall-2111`.

| Metric | Current evidence | What is still pending |
| --- | --- | --- |
| Detection to effective restriction | PASS: 37 scored restrictions across fault and nominal regressions, 0 late/unconfirmed; worst effect latency 11.958 ms. Stricter supervisor-receipt latency also passes: worst 12.856777 ms. Manager-stall subset: 30/30, worst receipt 11.653796 ms | Broader matched-load and interruption tests; other failure paths |
| Critical deadline misses | 0 misses in 22,394 archived jobs; max response 1.179685 ms against 5 ms. Fault-run response p99 0.643685 ms | Long-duration soak, actual application control algorithm, adversarial IRQ/system load |
| Thermal/workload overshoot | Score: simulated thermal 0.0 C in ramp run; real CPU worst 6 percentage points across runs; estimated-demand index 2 points, in existing 100 ms windows | Real thermal peak/cooldown capture; declared acceptable overshoot bounds |
| State-transition jitter | Current nominal regression: Normal-to-Warning spread 0.768 ms, n=2. Prior revision baseline: 5.099 ms, n=30 | Repeat the full nominal 30-trial series with the independent enforcer; declare acceptance bound and cover other transitions |
| Controlled restoration | Worst 3,057.432 ms across 35 current fault/nominal recoveries, including declared 2 s plus 1 s dwell | Longer repeated fault/recovery characterization and accepted maximum additional delay |

The current nominal scorecard contains 6,032 reported jobs and its archive
contains 6,031 because reads end at different instants. The stall-run archive
and scorecard both contain 16,363 jobs. Each raw archive is contiguous from ID 1;
both scorecards report zero missing records. These are separately scoped counts.

The prior nominal 30-trial baseline is `results/threadctl-pi-validation/repeat30/` and target
`threadctl-results-14065727`. Its scorecard read contains 13,689 reported jobs;
the recorder continued through ID 13,690. Neither stream reports a gap.

The quiet thermal characterization acquired all 120 planned real endpoint reads
over 3 s at 25 ms intervals, with zero missing slots, overflows, read errors or
invalid CPU windows. Its sampler plus receiver used approximately 0.127% of one
core during this short quiet run. This is not the total stack's CPU overhead.

The 20 ms metric begins at a qualifying sampled threshold, sensor expiry or
manager-progress expiry. The manager timeout remains 50 ms; worst observed
SIGSTOP-to-receipt was 61.251740 ms, while expiry-to-receipt was below 20 ms.
A CPU injection also includes the declared 100 ms utilization window. These
results do not claim 20 ms from arbitrary fault or load injection.

## Fault evidence and remaining testing

An earlier integrated capture (`results/threadctl-pi-validation/integration/`)
passed ramp/spike/freeze and both service-stall assertions, with 10,176 archived
critical jobs and zero misses/gaps. Its combined restriction score **failed**:
13 confirmed, 2 late, worst 1,029.211 ms. This capture deliberately paused the
manager and watchdog for one second and used an earlier timing configuration.
It is retained as fault evidence, not substituted for the final nominal score.

The previous revision's fault-only capture is `results/threadctl-pi-validation/faults/`
and target `threadctl-results-19492928`. Manager-stop and watchdog-stop both
closed the workers; both recovered to Normal after resuming and satisfying the
clear observation. All assertions passed, with 1,619 archived critical jobs,
zero gaps/misses and maximum response 1.285759 ms. Its 20 ms restriction score
**failed** during deliberate service suspension: 2 confirmed, 1 late,
worst 967.984 ms. That failure is retained as the before-fix baseline.
The corrected independent enforcer now confirms effects while the manager is
stopped, with the 30-trial passing results above. Effect reporting cannot renew
manager progress or clear its fault. Recovery still requires fresh progress,
valid telemetry and the original continuous dwell.

- [x] Four CPU affinity checks and child inheritance on the actual Pi.
- [x] Invalid-mask and mask-restoration assertions on the actual Pi.
- [x] Gradual simulated temperature ramp and ordered restriction.
- [x] Real noncritical CPU-load spike with labelled simulated temperature.
- [x] Stale-temperature fail-safe and staged restoration.
- [x] Real onboard endpoint read in quiet characterization mode.
- [x] Archive prior 30 nominal-spike trials and the new 30 manager-stall trials.
- [x] Recheck manager/watchdog stalls on the final timing configuration.
- [x] Fix manager-stall confirmation within 20 ms of its declared progress expiry and verify receipt before SIGCONT in 30 Pi trials.
- [ ] Repeat the nominal 30-trial transition series with the new enforcer and freeze a jitter acceptance bound.
- [ ] Test worker death, coordinator crash/hang and restart/requalification.
- [ ] Test enforcer crash/hang, takeover/requalification and its independent lease-expiry path.
- [ ] Test affinity/scheduling failure under the intended non-root account and abilities.
- [ ] Verify physical LEDs, wiring and correspondence to the CLI.
- [ ] Integrate one real thermal producer with the separate-process stack; current judging scenarios use a simulated temperature producer.
- [ ] Validate frozen-conversion handling against the actual sensor's conversion identity; repeated file reads alone do not establish fresh conversions.
- [ ] Capture real thermal overshoot through the post-restriction peak and cooldown.
- [ ] Freeze acceptable jitter, overshoot and extra-restoration bounds.
- [ ] Run a longer critical-task soak with representative system interference.
- [ ] Qualify total stack supervision CPU overhead, rather than treating the monitor-only measurement as a system result.
- [ ] Run a matched comparison to quantify affinity's effect; no improvement claim from affinity alone.
- [ ] Validate the Pi 5/BSP separately.

Hardware power metering and dynamic frequency control remain optional stretch
goals and are not part of this ThreadCtl implementation.
