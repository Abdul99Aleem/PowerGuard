# PowerGuard supervisor validation and testbench handoff

19 November 2026. Branch `feature/PG-14-powerguard-supervisor`, base commit `ceee8c4`, with timestamped Console logging in the working tree.

## Implementation

The QNX service receives samples and acknowledgements through native message passing. A 10 ms monotonic timer also wakes the supervisor when the sensor is silent. The policy limits NC2 before NC1 and restores NC1 before NC2. Safe observations require temperature at most 60 °C, busiest-core occupancy at most 60%, and power index at most 55. Temperature may be up to 250 ms old and CPU telemetry up to 50 ms old. Each release step requires manager confirmation and 500 ms of continuous headroom. An expired manager heartbeat or invalid telemetry triggers conservative limits.

The Momentics console prints elapsed time, source, previous and next state, sensor age, CPU, power index, NC budgets, decision ID and effect status. QNX trace user event 80 records policy changes in the service. Critical job deadline misses are scored independently of the recovery gate.

## Execution record

Rebuilt PowerGuard, pg-console and pg-check for QNX AArch64 Debug with warnings treated as errors. The Windows host ran pg-check, pg-console --example and a separate interrupted-recovery scenario. The raw logs and SHA-256 fingerprints of the three target binaries are in the local results directory `results/2026-11-19-supervisor-validation/`.

SSH reached the supplied board endpoint on port 22. Git for Windows OpenSSH completed key exchange, but the server rejected the default workstation account with `Permission denied (publickey,password)`. The subsequent authenticated board run is recorded below.

## Captured five-metric scorecard

All values below are from the simulated host example. Its mock manager supplies a synthetic 1 ms effect. The example deliberately supplies a late critical job.

| Metric | Host result | Live evidence the testbench must supply |
|---|---|---|
| Detection to effective restriction | 5 confirmed, worst 1.000 ms against provisional 20 ms | Match the stricter decision ID to the manager timestamp when both worker caps took effect. Retain unconfirmed and late actions. |
| Critical deadline misses | 1 of 2 reported jobs, max completed latency 12.000 ms | Send each ideal release, relative deadline and finish, in order. Report overdue unfinished jobs too. |
| Overshoot | 6.0 °C thermal and 1 CPU percentage point | Retain timestamped samples during the 100 ms window after the triggering restriction. Record whether the source is simulated or onboard. |
| Transition jitter | 0.000 ms across 2 Normal-to-Throttled trials | Repeat the same initial state, trigger and requested transition. Report maximum minus minimum transition latency and sample count. |
| Controlled restoration | 3 completed episodes, latest and worst 2031.000 ms | Timestamp the start of uninterrupted safe conditions and confirmation of full effective budgets. |

For the interrupted-recovery check the inputs were `55 86 30`, `55 30 30 good 600`, `55 61 30 good 600`, then `55 30 30 good 1500`. The 61% reading held NC2 at 50%. The final safe interval restored NC2 to 100% and the state returned to Normal.

## Connect the existing testbench

1. Start the QNX service under its configured FIFO priority. Connect on the Pi to the name defined by PG_NAME in include/ipc.h. Use fixed-size pg_message_v1 messages, PG_VERSION and target CLOCK_MONOTONIC nanoseconds. Read PG_READ for the current epoch.
2. Publish ordered PG_SAMPLE records with acquisition sequence, acquisition time, quality, source, CPU window end, four core occupancies and two pre-cap demands. PG_SAMPLE_POWER additionally accepts an explicit index in reserved. Keep original acquisition timestamps when simulating stale data.
3. Feed actual NC1/NC2 commands to the workers. Only after application, send PG_EFFECT with epoch, decision ID, both caps and their effective timestamp. Repeat the matching confirmation within the 50 ms lease. The independent watchdog and worker expiry belong in the integrated rig.
4. Arm PG_TRIAL just before each comparable injection. Send PG_JOB records from the timing reporter. Read PG_SCORE using the matching series and state pair. Start scored restrictions after reaching initial Normal.
5. First board smoke test: run pg-check and pg-console --example. Both exercise synthetic scenarios even on a Pi. Then exercise live IPC with the manager, workers, watchdog and critical task and retain the trace and raw job/effect logs.

## Raspberry Pi deployment and target results, 20 November 2026

SSH with qnxuser, AES-128-CTR and HMAC-SHA2-256 reached host qnxpi. The board reported QNX 8.0.0, RaspberryPi4B, aarch64le. The three debug binaries were transferred to /tmp/pg-smoke-4341812 and byte counts checked. This directory is temporary.

On the board, pg-check printed PASS for the metric calculations, explicit power input and policy replay. pg-console --example produced the same five synthetic values shown in the table above. The decision code executed on the Pi; the example generated its own timestamps, jobs and worker acknowledgements.

Starting PowerGuard at FIFO priority 20 as qnxuser failed during high-resolution timer setup with “policy timer: Operation not permitted”. A bounded privileged smoke run succeeded using sudo -n with timeout 8 and PowerGuard 20. During that window pg-console --live scores successfully queried the real service through QNX IPC as qnxuser (client exit 0). The service was time-limited for this check. The live report had zero confirmed restrictions, no job records, no threshold samples, zero comparable jitter trials and no completed restoration episode. The external workload testbench is the next dependency for live timing measurements. For deployment, configure the launch identity with the required QNX timer ability.

Local evidence in results/board-20261120-000540:
- board-output.log records target identity, deployment path, target-side tests and simulated example.
- live-output.log records the ordinary account timer error.
- bounded-output.log and bounded-exit.txt record the successful live IPC query and exit code.
