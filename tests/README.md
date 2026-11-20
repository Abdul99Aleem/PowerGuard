# Regression suite

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
