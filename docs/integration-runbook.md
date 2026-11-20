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
