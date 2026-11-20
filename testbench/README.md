# PowerGuard minimal QNX testbench (branch: feature/ham-testbench-cli)

This is an **isolated application**, not a replacement for the team's policy supervisor. It provides a critical periodic worker, NC1 and NC2 CPU burners, simulated fault scenarios, a QNX message-passing CLI, CSV measurement, and an **optional HAM launcher**. It does not alter the root `Makefile`, `.project`, `.cproject`, or the supervisor.

## Build on QNX SDP 8 / Momentics host

Open a QNX SDP 8 environment with `qcc` on PATH, then from this directory:

```sh
make
```

This builds `pgbench` and `pgbench_ham` for `aarch64le` (QNX Pi 4/5). If necessary use `make ARCH=aarch64le`. Copy executables to the QNX target via your usual Momentics deployment or `scp`. Do not try to run the QNX binaries on Windows.

The default root Momentics project builds `src/PowerGuard.c` only; **build this subproject separately** with its Makefile, or add a separate C/C++ Makefile project in Momentics referencing `testbench/`.

## Demo without HAM (start here)

Run on target in one terminal:

```sh
mkdir -p /tmp/powerguard
./pgbench serve /tmp/powerguard/bench.csv
```

In another terminal:

```sh
./pgbench status
./pgbench scenario ramp
./pgbench status
./pgbench level 1              # supervisor mock: restrict NC2 first
./pgbench level 2              # supervisor mock: restrict NC1 too
./pgbench scenario freeze      # sensor stops refreshing after 4 s
./pgbench status               # sensor age starts growing after freeze
./pgbench state critical 0     # supervisor-reported state (not autonomous)
./pgbench scenario normal
./pgbench state recovery 50
./pgbench level 1              # restore NC1 first
./pgbench level 0              # restore NC2, measure recovery-to-full
./pgbench state normal 100
./pgbench stop
```

The server periodically appends `bench.csv`, once per second, with the following fields: monotonic timestamp, scenario, simulated temperature, sensor age, process CPU time as a proportion of one core, requested/applied restriction, reported supervisor state, critical job count, misses, maximum response time, maximum release jitter, time to worker acknowledgments, and recovery-to-full. This is **raw demonstration evidence**, not a guarantee of hardware timing.

## Optional: HAM-managed process restart

HAM adds fault recovery, not thermal budgeting. Start the QNX HAM service according to your BSP/startup environment. Then register a **new**, uniquely named entity:

```sh
./pgbench_ham pg_bench_demo '/absolute/path/pgbench serve /tmp/powerguard/bench.csv'
```

HAM starts the process, attaches it as an entity, and registers a rearmed **CONDDEATH → restart** action. Run `./pgbench status` from another terminal; terminating the pgbench process unexpectedly should trigger HAM restart. Do **not** also manually run a second server, and do not register another entity of the same name. HAM can restart the entire process; these workers are threads, so it does **not** independently monitor NC1/NC2. The demo's CSV will be truncated on a process restart: use per-run filenames or archive runs before failure testing.

## Interface for teammate's supervisor

Include `pgbench_protocol.h` in an external QNX process. Use `name_open(PG_BENCH_NAME, 0)` then `MsgSend()` with `pg_request_t`, receive `pg_status_t`, and `name_close()`. Commands:

| Command | Arg | Purpose |
|---|---|---|
| PG_CMD_STATUS | unused | Snapshot telemetry |
| PG_CMD_SCENARIO | 0 normal, 1 ramp, 2 spike, 3 freeze | Inject test inputs |
| PG_CMD_LEVEL | 0 both on, 1 NC2 off, 2 both off | Enforce supervisor command on testbench workers |
| PG_CMD_STATE | 0-5 state, extra=0-100 recovery percent | Annotate supervisor policy decision |
| PG_CMD_STOP | unused | Stop testbench |

Supervisor should send LEVEL **after its own real policy decision**, not based on a simulated state command. To measure true limit-detection-to-enforcement latency, timestamp sensor-trigger detection in the supervisor, then combine it with a later worker-ACK timestamp using the **same CLOCK_MONOTONIC base**; `last_actuation_us` in this MVP is **receipt of LEVEL to worker acknowledgement**, not detection-to-throttling. The protocol reserves `detection_to_actuation_us` but this MVP leaves it zero until actual supervisor timestamps are integrated.

## Live dashboard

With `pgbench serve` running, launch `./pgbench dashboard` from an interactive
terminal. It continuously shows the required simulated temperature and sensor
age, CPU, NC1/NC2 caps, policy state, recovery progress, critical timing and
worker acknowledgement timing. Use Up/Down to select Normal, Ramp, Spike or
Freeze; press Enter to apply the selected scenario and `q` to quit. The
dashboard changes testbench scenarios through QNX IPC, so it remains compatible
with `pg-manager`; do not inject competing numeric samples through `pg-console`
while the manager is running.

## Measurement definitions and limitations

- The critical worker releases every 10 ms with a 5 ms relative deadline and approximately 1 ms of synthetic work. The bench counts late jobs and skipped releases. The supervisor bridge forwards the latest job and exposes gaps in the records it received.
- NC1 approximates a 20% duty cycle per 100 ms; NC2 15%, or 90% in spike scenario. Exact percentages vary under scheduling contention; work is simulated CPU demand, not power watts.
- `process_cpu_per_core_tenths` uses `CLOCK_PROCESS_CPUTIME_ID`: process CPU consumption relative to **one core**, not whole-system CPU usage. It may exceed 100% for a multithreaded process on multiple cores; it must not be labeled global CPU utilization.
- Temperature is software-generated, in m°C. The ramp rises 1°C/s to +35°C over baseline. Freeze updates four samples then leaves the same sample and its original timestamp; sensor age should grow.
- The supervisor supplies state and recovery annotations. Workers start closed and enforce the expiry of managed grants locally. The independent watchdog, board telemetry, and target timing qualification are separate integration tasks.
- NC levels are cooperative workload suspension and acknowledgement, **not OS-enforced budgets or a process stop**. Acknowledgment is observable but not equivalent to resource manager enforcement.
- The critical worker *attempts* SCHED_FIFO priority 20 and reports whether it was granted; do not claim deterministic priority behavior if it falls back.
- Optional HAM requires a running, configured HAM daemon, a reachable `libham`, adequate permissions, an executable absolute path, and suitable boot image.
- No QNX SDP / Pi runtime is available here to compile or benchmark this implementation. **Build and run on the actual target before presenting measurements to judges.**

## Suggested three demos

1. `scenario ramp` → supervisor chooses LEVEL 1 then LEVEL 2 → inspect actuation and deadline misses.
2. `scenario spike` → observe the process CPU demand rise → supervisor LEVEL 1 suppresses NC2.
3. `scenario freeze` → after four samples, age grows → supervisor chooses conservative LEVEL 2 and later staged restoration.

QNX HAM API references: [HAM quick example](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.ham/topic/examples_SIMPLE.html) and [name_attach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html).
