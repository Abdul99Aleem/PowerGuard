# Real temperature integration

This mode connects the existing `/dev/thermal` pulse reader to the split runtime.
Implemented on `feature/live-thermal-integration` and verified on the QNX 8
Raspberry Pi 4B on 21 November 2026. The original simulated scenarios remain
available separately for reproducible thermal-ramp tests.

## Verified live result

| Check | Recorded result |
| --- | --- |
| Physical sensor | 730 `/dev/thermal` acquisitions, 52.1–55.5 °C, REAL/READ_ONLY source |
| Pulse reader | 25 ms period, zero read errors, zero overflow, zero invalid CPU windows |
| Managed CPU spike | Throttled, NC1 100% / NC2 0%; simulated thermal ramp rejected |
| Paused real producer | Critical, NC1 0% / NC2 0%; manager and critical-job reporting remain active |
| Fresh input and recovery | Returns to Normal with both workloads available after controlled dwell |
| Restriction timing | Two confirmed, zero late/unconfirmed; worst effective restriction 8.977 ms against 20 ms |
| Critical jobs | 2,135 captured jobs, zero misses/gaps/period errors; maximum response 0.986500 ms against 5 ms |
| Restoration | Two recoveries; worst 3049.986 ms including 2 s + 1 s observation |

**[Raw live evidence](validation/real-thermal-pi4-evidence.zip)** ·
**[Machine-readable summary](validation/real-thermal-pi4-summary.json)**

The operator's later [SSH repeat](ssh-validation-followup.md) also passes and
has its own downloadable raw capture. To demonstrate **physical temperature
activation**, use the separately labelled [low-temperature profile](low-temperature-demo.md),
and follow the [System Profiler screenshot guide](system-profiler-screenshots.md).

The recorder ends at job 2,135; the earlier scorecard ends at 2,130. These are
different endpoints, both with zero internal gaps. The sensor stayed below
65 °C, so this run establishes the real input path, CPU restriction and producer
freshness behavior, **not physical thermal-threshold overshoot or cooldown**.
The scorecard has no matching transition-jitter trials in this short run.

The first run passed live state/timing checks but its script exited unsuccessfully
because intentional monitor shutdown still triggered the standalone full-duration
capture rule. The runtime sink now records `interrupted=1`, preserves its actual
counts, and treats requested partial shutdown as successful only if read/error,
overflow and CPU-window checks pass. The complete acceptance run was repeated
successfully. Initial and final captures are retained in the evidence bundle.

The 120 s acquisition allocation is deliberately stopped after the acceptance
sequence. `missing=4070` includes the unrun allocation and the deliberately
paused producer; it is not a claim of 120 s uninterrupted acquisition. The final
capture contains 730 actual readings; no acquisition error is suppressed.

The original simulated stack was also rerun after this change with one scenario
cycle, three manager stalls and two manager restarts. All target assertions
passed; 7,853 raw critical jobs had zero misses/gaps and all three manager-stall
receipts met the 20 ms bound (worst 12.137723 ms). The supervisor watchdog latch
was observed within 221.423092 ms against its separate 270 ms limit. This is a
short compatibility regression, not a replacement for the earlier 30-trial
campaign. Its deliberate post-supervisor-stop late receipt remains in the bundle.

All eight recorded release-binary hashes were checked against the deployed Pi
copies. Native affinity, policy, manager, latch and thermal-bridge checks passed.
The run stopped its own stack; no test workload was left running.

## Data path

`/dev/thermal → pg-thermal (25 ms pulses) → runtime thermal input → manager → supervisor → independent enforcer → managed NC workers`

The manager remains the sole policy sample producer. It combines the actual
acquisition timestamp and READ_ONLY/error quality with measured per-core CPU,
declared NC demand, critical jobs and manager progress. A paused temperature
producer cannot be hidden by a running manager: forwarding does not renew the
acquisition timestamp.

Set `PG_REAL_THERMAL=1` **before starting the runtime**. It starts without a
valid temperature, never generates synthetic temperatures, rejects simulated
ramp/freeze commands, and accepts thermal updates from only one live producer
PID. Normal and spike commands still change managed workload demand. The
existing protocol-v4 status layout is unchanged; the thermal request and source
feature bits are additive. Rebuild all related binaries together.

The monitor's new `runtime` sink forces 25 ms pulse acquisition, publishes every
sample and disables its standalone heater. Use the managed NC workers as load.
Device open/read runs in the monitor process, outside the coordinator and
supervisor. Reader blockage/death becomes stale input; there is no fallback to
simulated data. CPU/thermal thresholds, 150 ms freshness, 35 ms recovery gap,
2 s + 1 s dwell, critical placement and enforcer behavior remain unchanged.

## Build and target acceptance

```sh
make BUILD_PROFILE=release CCFLAGS='-Wextra -Werror'
make -C testbench
sh scripts/check-host.sh
```

Deploy `PowerGuard`, `pg-runtime`, `pg-manager`, `pg-enforcer`, `pg-thermal`,
`pg-console`, `pg-validate`, `pgbench` and `scripts/validate-real-thermal.sh`
together in a **new** directory on the Pi. Confirm `/dev/thermal` units and the
four procnto idle-thread IDs on this boot. For Celsius and verified IDs 1–4:

```sh
sh validate-real-thermal.sh C 1 2 3 4
```

The prepared, verified bundle for this run is at
`/tmp/pg-real-thermal-1795224989` on the current Pi. From a connected Pi terminal,
enter that directory and run the command above. It contains the updated binaries;
the earlier `/tmp/pg-t3-merge-2111` bundle remains untouched.

This starts a private instance, checks REAL-source Normal, applies managed CPU
load and checks restriction, returns to Normal, pauses only the thermal monitor,
checks Critical with both NC workloads off, resumes and checks recovery. Status,
jobs, scorecard and raw thermal samples are saved in `real-thermal-evidence-<pid>/`.
Only this test's processes are stopped. Both initial qualification and the final
scorecard must pass; failed startup is not a successful demonstration.

After downloading evidence, run `python tests/analyze_threadctl.py <run>/jobs.csv`
and inspect the monitor CSV's physical values. CPU load may trigger restrictions
before the board reaches 70 °C; a passing CPU/freshness test does not establish
physical thermal-limit overshoot. Thermal overshoot needs an actual physical
threshold crossing and a complete peak/cooldown capture.

## Sensor limitation retained

The accepted device format exposes a value, not a hardware conversion counter.
READ_ONLY explicitly distinguishes read freshness from conversion freshness.
The existing 2 s unchanged-value rule remains conservative and can close work
even when the sensor is genuinely stable. Do not relax that rule simply to get
a passing run, or claim physical frozen-conversion qualification from a paused
producer test. Simulated ramp/freeze scenarios remain useful, separately labelled
reproducible policy tests.
