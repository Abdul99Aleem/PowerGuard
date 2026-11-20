# Integrated QNX runbook

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
Rebuild pg-manager and pgbench together for bench protocol v2. The supervisor remains on PG_VERSION 1. Workers start closed. The manager renews absolute 50 ms grants and the workers reject expired or superseded grants. A stalled manager cannot renew a grant. In a managed run, use scenario commands to change inputs. Manual level commands are for a standalone bench. An independently verified source and genuine per-core measurements still need to be supplied by the board monitor. The current testbench reports an explicitly simulated temperature and uses process CPU as its load proxy.
