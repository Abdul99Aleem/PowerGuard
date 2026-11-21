# Regression suite

`thermal_demo_checks.c` also verifies the separate demo profile's activation
boundaries, ordered workload caps and full recovery dwell. It is built with
`PG_THERMAL_DEMO` by the host script and QNX `thermal-demo` target. The standard
policy regression remains compiled with the original thresholds.

For all portable checks with a C11 compiler and Python 3, from the repository:

```sh
sh scripts/check-host.sh
```

This runs policy/metric replay, manager-progress and immutable-receipt checks,
watchdog-latch and negative benchmark checks, and Python evidence-analysis
tests. Outputs are ignored under `build/host-checks/`. Set `CC` or `PYTHON` to
an alternate compiler/interpreter executable when needed. These checks do not
qualify QNX scheduling or physical hardware.

`thermal_checks.c` exercises the real thermal bridge's replay, future timestamp,
quality and range rejection; source isolation; stale-input closure; and fresh
data recovery through the actual policy. For board acceptance, deploy and run
`scripts/validate-real-thermal.sh` as described in the
[real thermal guide](../docs/real-thermal-integration.md).

For the separate-process ThreadCtl runtime and the manager-stall fix, see the
[complete architecture and problem-resolution guide](../docs/powerguard-design-and-manager-stall-fix.md)
and [Pi validation report](../docs/manager-stall-fix-validation.md).
`pg-threadctl-check` tests the QNX thread-control interfaces;
`pg-manager-check` checks progress expiry and immutable receipt timing;
`pg-stall-check` measures supervisor confirmation while the manager remains
SIGSTOP'ed. The updated `scripts/validate-threadctl.sh` orchestrates these tests
with isolated service names. Run it from a directory containing the full
bench-protocol-v4 bundle, including `pg-enforcer`.

`pg-check` is the deterministic regression executable. It runs policy and
metric assertions without a board and covers startup fail-safe behavior,
thermal thresholds, one-core CPU overload, estimated-power overload,
stale/frozen and malformed telemetry, manager-lease expiry, rejected effect
acknowledgements, staged recovery, deadline accounting, transition jitter,
overshoot and the supervisor-to-testbench state mapping.

Build it with the root project. On a QNX target run:

```sh
/tmp/pg-check
```

The live regression smoke sequence must use the integrated stack, then record
the scorecard and `/tmp/pgbench.csv` for each case:

```sh
/tmp/pgbench scenario normal; sleep 4; /tmp/pg-console --live scores
/tmp/pgbench scenario ramp; sleep 35; /tmp/pg-console --live scores
/tmp/pgbench scenario spike; sleep 3; /tmp/pg-console --live scores
/tmp/pgbench scenario freeze; sleep 6; /tmp/pg-console --live scores
/tmp/pgbench scenario normal; sleep 6; /tmp/pg-console --live scores
```

These inputs are explicitly simulated. Preserve raw output and do not present
them as measured hardware-temperature or power evidence.
