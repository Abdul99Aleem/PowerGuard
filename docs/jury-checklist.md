# Jury checklist · Problem 16

**PowerGuard: Deterministic Power and Thermal-Budget Management for Edge Systems**

**Team Schedulix · QNXHACK-HYD-014**

This page maps the supplied problem statement to implementation, demonstration
and evidence. “Pi-tested” refers to the archived QNX 8 Raspberry Pi 4B campaign,
not a new test performed by opening this document.

## Requirement coverage

| Problem requirement | Implementation / answer | Where to inspect or demonstrate |
| --- | --- | --- |
| One critical and two noncritical workloads on one Pi | Separate critical, NC1 and NC2 processes; verified CPU1/2/3 placement and FIFO30/10/8 | `runtime/worker.c`, `runtime/runtime.c`; demo **1** shows readiness, identities and caps |
| Temperature, estimated power or CPU activation limits | Highest-severity input wins; explicit numeric activation and recovery thresholds | [README policy table](../README.md#declared-policy), `supervisor/policy.c` |
| Restrict in a predefined order | Throttled closes NC2; Critical closes both; critical control receives no NC cap | `ordered_cap` in `runtime/runtime.c`; demo **2**, **3** |
| Preserve critical response deadlines | Critical remains running at verified placement/priority; complete release records reveal misses and gaps | 10 ms period, 5 ms deadline; demo **1**, **7**; [job evidence](validation/teammate3-merge-pi4-summary.json) |
| QNX message-based communication | Versioned supervisor/runtime protocols; bounded exchanges, sample/job forwarding and effect receipts | `include/ipc.h`, `testbench/pgbench_protocol.h`, `manager/pg-manager.c`, `runtime/enforcer.c` |
| High-resolution timers | Monotonic expiry pulses, absolute worker releases and timer tolerance requests | `src/PowerGuard.c`, `runtime/worker.c`; [API guide](threadctl-integration.md) |
| Priority-based scheduling | FIFO priorities and ThreadCtl affinity are set and read back; readiness fails if qualification fails | `platform/threadctl.c`; `pg-threadctl-check` |
| Sensor-freshness validation | Acquisition timestamps, increasing identities, quality and age checks; invalid data forces both NC workloads off | `supervisor/policy.c`; demo **4**; temperature age limit 150 ms |
| Five explicit states | Normal, Warning, Throttled, Critical, Recovery; conservative Critical startup | `include/policy.h`, `supervisor/policy.c`; CLI state and cause |
| Separate recovery thresholds | All three inputs must remain below lower clear limits; acknowledged caps, fresh samples and continuous dwell gate restoration | Demo **5**; [policy explanation](source-guide.md#supervisor-and-policy) |
| Gradual thermal increase | Labelled simulated ramp through 65/70/75 °C | Demo **3**, about 32 s; then **5** |
| Sudden noncritical CPU spike | Real synthetic NC2 computation with measured per-core utilization | Demo **2**; utilization uses a 100 ms sliding window refreshed every 10 ms |
| Stale or frozen temperature input | Freeze stops acquisition progress; replay cannot renew sample identity or timestamp | Demo **4**; after freshness expiry expect conservative Critical |
| Progressive controlled restoration | From Critical: NC1 after 2 s clear, NC2 after another 1 s; from Throttled: NC2 after 3 s | Demo **5**; unsafe input or an acquisition gap over 35 ms interrupts observation |
| Temperature Monitor | Original menu uses simulated temperature; separate real-input acceptance is now Pi-tested | [Real thermal results and reproduction](real-thermal-integration.md); `telemetry/thermal.c` |
| Supervisor, Workload Manager and watchdog | Separate supervisor, manager, runtime watchdog and independent enforcer | [Architecture](../README.md#how-qnx-makes-the-design-work), [process placement](threadctl-integration.md#process-placement) |
| Mandatory CLI fields | Temperature/source, age, CPU, applied/requested workload caps, state, restriction cause, critical latency, misses, job gaps and recovery clear time | Demo **1**; `scripts/jury-demo.sh` `show_status`; `pg-runtime status` and `pg-console --live scores` |
| Five physical state LEDs | GPIO observer implemented; physical validation **pending** | `gpio/pg-state-leds.c`; LED procedure below |
| Optional hardware power / frequency control | Neither required for the core demo; no measured-watt or DVFS claim | Demand index defined below; hardware metering and DVFS remain stretch goals |

## Timing contract: what starts the clock?

The restriction target is **20 ms from qualifying sampled limit detection or
an input/progress expiry**, with both worker-effective and first accepted
supervisor-receipt timestamps retained. It is not 20 ms from an arbitrary
external injection command. CPU spikes include the utilization window before
the measured threshold is detected. A manager stall first reaches its fixed
**50 ms progress timeout**; the additional receipt bound is then 20 ms.

Sensor expiry uses the original acquisition time plus **150 ms**. Repeated old
samples do not move that origin. A READ_ONLY thermal endpoint exposes no actual
conversion identity; unchanged values for 2 s trigger conservative handling,
which can also flag a genuinely stable sensor. A physical-conversion freshness
claim requires further hardware qualification.

Supervisor loss has a separate contract: shared progress expires after **200 ms**,
the independent watchdog checks every **50 ms**, and the test requires observed
latched off behavior within **270 ms from last progress**. Ordinary grants and
supervisor resumption cannot clear this latch. Demo **9** creates a new run.

## All five scored metrics

| Metric | Definition | Current result and practical limit |
| --- | --- | --- |
| Limit-detection-to-throttling response | Worker-effective timestamp minus qualifying detection/expiry; receipt measured separately | Pre-watchdog: 36 restrictions, worst effect 13.820500 ms, worst receipt 14.798204 ms. Manager-stall subset: 30/30, worst receipt 12.135167 ms |
| Critical-task deadline misses | Completion response relative to scheduled release and 5 ms deadline; job IDs detect missing reports | 21,601 raw jobs, zero misses/gaps; maximum response 1.148518 ms. Synthetic control workload and finite capture |
| Thermal or workload overshoot | Maximum observed excess over activation limit in 100 ms score windows | Simulated thermal 0.0 °C, CPU 6 percentage points, demand 2 index points; physical peak/cooldown and accepted bounds remain unqualified |
| State-transition jitter | Spread of transition latency across the same armed case and state pair | Normal → Warning 2.051 ms at n=2; insufficient for a broad jitter-bound claim |
| Controlled-restoration time | Continuous clear interval through acknowledged full restoration | Worst 3057.061 ms over 35 recoveries; includes 2 s + 1 s dwell |

The supervisor SIGSTOP test intentionally prevents prompt receipt and retains
one late restriction (959.400 ms) in the post-fault scorecard. Its independent
watchdog gate/latch test passes; that does not make the delayed receipt pass.
Show demo **7 before 8**, and retain both results.

## What is real, simulated, or estimated?

| Item | Submission scope |
| --- | --- |
| CPU workload and utilization | Real computation and measured per-core idle-time accounting on the Pi |
| Temperature in original jury scenarios | Explicitly SIMULATED; separate [real-input test](real-thermal-integration.md) passes with 730 physical readings; physical thermal threshold crossing is still unqualified |
| Power | Integer demand index `(2 × NC1 request + 3 × NC2 request) / 5`, or explicit 0–100 index; no watt measurement |
| Critical control | Synthetic periodic work, not a deployed physical control algorithm |
| Restriction effect | Worker acknowledgements and immutable supervisor receipt, not just a command being sent |
| Pi support | Integrated evidence is Pi 4B; Pi 5/BSP must be qualified separately |

## Five-state LEDs: implementation and pending board check

The observer uses BCM GPIO **17 Normal, 27 Warning, 22 Throttled, 23 Critical,
24 Recovery**; these are GPIO identifiers, not physical header pin numbers.
Verify driver availability, 3.3 V wiring and current-limiting resistors. Deploy
`pg-state-leds` and run `./pg-state-leds --board pi4` with the **same `PG_INSTANCE`
as the demo** and required GPIO access. The demo prints its instance name;
export it in the observer's terminal before launch.

Check each state against CLI output, record board/driver and wiring, and retain
visible evidence. The demo wrapper does not start the GPIO observer. On a lost
supervisor connection the observer clears its displayed LED. Physical LEDs
remain a mandatory open qualification item until this check is performed.

## Evidence and reproduction

1. Follow the [build/deploy manifest](jury-runbook.md#build-and-prepare-another-copy).
2. Demonstrate ramp, spike, freeze and recovery; retain CLI scores and the
   wrapper's jobs, samples and fault receipts.
3. Download the [current raw evidence bundle](validation/pi4-integrated-evidence.zip),
   follow its [analysis instructions](validation/README.md), and compare with
   the [current report](teammate3-merge-validation.md) and
   [JSON summary](validation/teammate3-merge-pi4-summary.json).
4. Use the [qualification checklist](threadctl-checklist.md) for unresolved
   hardware, interference, jitter, overhead and failure-path tests.

The committed current-run bundle contains 26 selected raw files and their hash
manifest, including the deliberate late-receipt scorecard. The broader local
campaign and earlier failed attempts remain on the paths named in the report.
The teammate HAM-off ZIP is a separate historical evidence set.
