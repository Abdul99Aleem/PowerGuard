# System Profiler screenshots for the jury

Keep the terminal scorecard alongside the trace: the scorecard/CSV establishes
deadlines and restriction bounds; a screenshot illustrates a selected interval.
Trace-on runs are diagnostic and must be labelled separately from trace-off
qualification measurements because tracing adds work.

## Capture an actual run

From the Pi directory containing the deployed binaries, start a uniquely named
kernel trace before starting the test. For the normal real-input acceptance:

```sh
trace_file=/tmp/powerguard-real-profile-$$.kev
tracelogger -s 45 -f "$trace_file" &
trace_pid=$!
sleep 1
sh validate-real-thermal.sh C 1 2 3 4
wait "$trace_pid"
echo "$trace_file"
```

Copy the printed `.kev` using the Momentics target file browser and open it in
**QNX System Profiler**. Use **Switch Pane → Timeline** and keep the time ruler,
process/thread names, CPU labels and state-color legend visible. Expand the
processes belonging to the new private instance, using that run's logged PIDs.
Do not mix rows from an older running demo.

For the low-temperature physical demonstration, use its directory and replace
the test command with `sh low-temperature-demo.sh C 1 2 3 4`; use a 90 s trace
to cover the bounded heater and cooldown. The supervisor's event **80** records
state/decision changes. Its payload is a binary `pg_status` structure, so named
states do not appear automatically without a user-event formatter. The standard
wrapper does not enable the thermal reader's optional events 81–84.

## Take these screenshots

| File | View and interval | What the jury should see |
| --- | --- | --- |
| `01-complete-flow.png` | Timeline, whole successful test; rows for critical, NC1, NC2, supervisor, enforcer, manager and monitor | Workload restriction and restoration while critical control continues |
| `02-critical-period.png` | Timeline zoomed to roughly 30 ms; critical thread on CPU1, visible ruler | Repeated critical releases around the declared 10 ms period; pair with the same run's job CSV for the 5 ms deadline claim |
| `03-restriction.png` | Zoom around the actual trip; NC1/NC2 and critical rows together | NC2 yields first; in Critical both noncritical compute workloads close while critical control continues. NC processes may still wake for reporting, so blocked/short control activity is expected |
| `04-progressive-recovery.png` | Roughly 4–6 s around clear input and restoration | Initial clear dwell, NC1 reopening, then NC2; match the same run's recorded caps and recovery progress |
| `05-pulse-wait.png` | Monitor main thread over roughly 100 ms; include IPC/thread-state events | Periodic wake-ups around 25 ms and blocked intervals between acquisitions, rather than continuous busy polling |

For `03`, state whether the trigger is **stale producer** (standard real-input
test) or **actual temperature above the demo limit** (low-temperature test).
Use the matching terminal temperature/source/age/cause output in a companion
shot; do not label a stale-input trip as physical overheating.

Also save one terminal screenshot containing the final PASS line and scorecard.
The System Profiler **CPU Usage** pane over the same selected interval can be
an optional sixth image, but it does not by itself prove total monitoring
overhead below a budget. Check that the trace has no lost/dropped events before
using it for timing interpretation. Retain the `.kev`, exact binary/profile,
run directory and CSVs with the images.
