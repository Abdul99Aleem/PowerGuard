# Regression suite

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
