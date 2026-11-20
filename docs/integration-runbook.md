# Integrated QNX runbook

For the ThreadCtl PRD implementation, use the separate-process `pg-runtime`
stack in [ThreadCtl integration](threadctl-integration.md). It provides verified
CPU placement, FIFO readiness, per-core idle-counter telemetry, a watchdog and
complete batched critical-job reporting. `pg-runtime status` consolidates the
operator view. The sequence below describes the retained legacy threaded bench;
it does not validate ThreadCtl placement. Bench protocol v4 requires rebuilding
pgbench, pg-runtime, pg-enforcer and pg-manager together. The ThreadCtl stack
requires pg-enforcer for confirmation independent of manager progress.

`pg-manager` is the connection between the policy service and the QNX
testbench. It publishes the testbench's explicitly simulated temperature and
process-CPU samples, forwards each policy NC1/NC2 cap to the workers, waits for
both worker acknowledgements, returns the matching `PG_EFFECT` heartbeat, and
reports the latest critical job timing record to the supervisor.

Build from the repository root and from `testbench/` with the QNX SDP 8
environment loaded. Copy these four AArch64 binaries to one target directory:

```sh
build/aarch64le-debug/PowerGuard
build/aarch64le-debug/pg-console
build/aarch64le-debug/pg-manager
testbench/pgbench
```

Start the three services in this order (the service priority and abilities must
be configured for the target):

```sh
./pgbench serve /tmp/powerguard/bench.csv &
./PowerGuard 20 &
./pg-manager &
```

Use `./pgbench scenario normal|ramp|spike|freeze` to inject only the labelled
simulated inputs, and `./pg-console --live scores` to read the supervisor's
five-metric scorecard. A successful host cross-build does not establish target
operation: retain target-side trace, scorecard and CSV evidence before making
timing or hardware claims.

## Five state LEDs

`make all` also builds `build/aarch64le-debug/pg-state-leds`. Copy it to
the target and run `./pg-state-leds --board pi4` or
`./pg-state-leds --board pi5`. The program needs the board's QNX BSP
GPIO utility on PATH and permission to drive the selected pins.
[Pi 4 gpio-bcm2711](https://www.qnx.com/developers/docs/BSP8.0/com.qnx.doc.bsp_raspberrypi.bcm2711.rpi4_8.0/topic/gpio-bcm2711.html)
[Pi 5 gpio-rp1](https://www.qnx.com/developers/docs/BSP8.0/com.qnx.doc.bsp_raspberrypi.bcm2712.rpi5_8.0/topic/gpio-rp1.html)

Reserve these five GPIOs for one instance of the LED process. For each LED,
connect the GPIO through a 330 Ω series resistor to the anode and connect
the cathode to a Pi ground pin.

| Supervisor state | BCM GPIO | Physical header pin |
| --- | ---: | ---: |
| Normal | 17 | 11 |
| Warning | 27 | 13 |
| Throttled | 22 | 15 |
| Critical | 23 | 16 |
| Recovery | 24 | 18 |

The process polls the supervisor about every 100 ms. Compare the lit LED
with the supervisor state reported by `./pgbench status` while holding each testbench state long enough
for a poll. During a state change it turns the previous LED off before
turning the next one on. Verify that all five go dark when the supervisor
is unavailable, that the current state appears again after the supervisor
restarts, and that all five go dark on Ctrl+C or SIGTERM.
Rebuild pg-manager and pgbench together for bench protocol v4. The supervisor remains on PG_VERSION 1 with additive progress/confirmation extensions. In this legacy bench the manager renews absolute 50 ms grants and workers reject expired or superseded grants. The ThreadCtl stack instead uses an independent enforcer and a separately timed manager-progress heartbeat. Use scenario commands for a managed run; manual levels are for a standalone legacy bench. The legacy testbench uses simulated temperature and process CPU, while pg-runtime supplies actual per-core utilization.
