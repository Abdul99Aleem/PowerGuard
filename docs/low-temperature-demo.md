# Low-temperature physical demonstration

This explicitly labelled demo executable uses lower temperature limits so the
Pi can demonstrate physical threshold crossings under a short, bounded load.
The standard executable retains its original limits. This profile demonstrates
the control flow; its small temperature bands are not production tuning.

| Profile | Recovery clear | Warning | Throttled | Critical |
| --- | ---: | ---: | ---: | ---: |
| `PowerGuard` (standard) | ≤62 °C | ≥65 °C | ≥70 °C | ≥75 °C |
| `PowerGuard-thermal-demo` | ≤56.0 °C | ≥56.5 °C | ≥57.0 °C | ≥57.5 °C |

Both profiles retain the 150 ms sensor-age limit, CPU/demand limits, 35 ms
maximum recovery sample gap and 2 s + 1 s recovery observation. The supervisor
prints its actual profile and limits at startup. A temperature below an
activation threshold does not instantly clear an existing restriction.

## Verified physical run · 21 November 2026

On the QNX 8 Pi 4B, instance `low_thermal_59875380` completed successfully:

| Evidence | Recorded result |
| --- | --- |
| Real temperature | 1,564 physical readings, **53.0–58.4 °C** |
| Complete flow | Normal, Warning, Throttled, Critical and Recovery all present in the 25 ms runtime capture; final Normal passes |
| Cause of restriction | Temperature-only Critical observed with `causes=0x1`, REAL source and fresh data; this was not the stale-producer test |
| Applied workloads | NC2 restricted first, then both off; NC1 restored before NC2 |
| Critical timing | **4,118 raw jobs**, zero misses/gaps/period errors, maximum **1.149482 ms** against 5 ms |
| Restriction timing | 2 confirmed, zero late/unconfirmed; worst **18.729 ms** against 20 ms |
| Thermal excess | Scorecard: **0.4 °C in 100 ms windows**. Whole-run peak 58.4 °C is 0.9 °C above the demo Critical limit; these are different observation scopes |
| Restoration | One scored recovery, **3055.535 ms** including 2 s + 1 s dwell after continuous clear conditions |

The scorecard ends at 4,113 jobs; the raw recorder ends later at 4,118. Both
have zero missing records. Small thermal fluctuations correctly restart the
clear observation before a qualifying uninterrupted interval. This is a short
bench demonstration, not a production thermal-limit or jitter qualification.

**[Raw evidence](validation/low-temperature-evidence.zip)** ·
**[Summary and binary hashes](validation/low-temperature-summary.json)**

The later full-kernel-trace run completed the same state flow with zero critical
misses, and exposed one 20.646278 ms restriction. It is tracked separately as
[PG-PERF-01](timing-investigation.md), including raw evidence and the plan to
increase timing margin. The passing figures above describe the trace-off run.

The prepared executable bundle is at **`/tmp/pg-low-thermal-1795226936`** on the
Pi. Exit any old demo before starting this one, then run:

```sh
cd /tmp/pg-low-thermal-1795226936
sh low-temperature-demo.sh C 1 2 3 4
```

The physical path is `/dev/thermal → 25 ms pulses → runtime → manager →
supervisor → independent enforcer → NC1/NC2`. The temperature remains REAL;
there is no generated ramp. Freshness/quality faults can independently cause
Critical, so a temperature-driven result must have the TEMP cause rather than
BAD_TEMP. NC2 closes before NC1 and NC1 restores before NC2; critical control
continues on CPU1 at FIFO30.

## Build and show the flow

```sh
make BUILD_PROFILE=release CCFLAGS='-Wextra -Werror' all thermal-demo
sh scripts/check-host.sh
```

`thermal-demo` builds `PowerGuard-thermal-demo`, `pg-thermal-demo-check` and
`pg-demo-heater`. Deploy those alongside `pg-runtime`, `pg-manager`, `pg-enforcer`,
`pg-thermal`, `pg-console`, `pg-validate` and `scripts/low-temperature-demo.sh`.
From that directory on the qualified Pi:

```sh
sh low-temperature-demo.sh C 1 2 3 4
```

The wrapper first requires Normal, then runs an **external test disturbance**
for 20 s at roughly 30% duty per core, FIFO1. This fixture heats the board; it
is not one of the application workloads that PowerGuard caps. It exits after
its fixed duration so actual cooling and staged restoration can be observed.
The CPU measurements still participate in policy; thermal evidence must be
distinguished from a CPU-triggered restriction.

The script records status, source/age/cause, applied caps, complete critical
jobs and physical readings in a private `low-thermal-evidence-<pid>/` directory.
It requires an observed temperature-only Critical, a subsequent Normal state,
passing critical-job assertions and restriction scoring, and a clean reader.
If the board cannot maintain the clear band, it correctly refuses restoration
and the demo fails rather than shortening the dwell or replacing the sensor.

An initial demo band used a 55 °C clear limit. Physical crossings occurred, but
idle-temperature fluctuations repeatedly interrupted recovery and the complete
run did not pass. The final demonstration band above was selected using that
observed board behavior. The original standard profile was not changed.

For trace captures and screenshots, use the [System Profiler shot list](system-profiler-screenshots.md).
For the standard-profile SSH run, see its [separate validation record](ssh-validation-followup.md).
