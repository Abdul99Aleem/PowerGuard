# PG-PERF-01 · Trace-on restriction margin

**Status: open performance investigation.** The physical demo meets its 20 ms
restriction target with tracing off. Full kernel tracing exposes a narrow
timing-margin issue: one of two restrictions took **20.646278 ms**, exceeding
the target by **0.646278 ms**. Critical control continued with zero deadline
misses. This is retained as a diagnostic result, not counted as a passing
restriction trial.

## Measured comparison

| Measurement | Trace-off physical demo | Full-trace diagnostic |
| --- | ---: | ---: |
| Worst effective restriction | 18.729 ms, within 20 ms | 20.646278 ms, 0.646278 ms above 20 ms |
| Restriction count | 2 confirmed, zero late | 2 confirmed, 1 late |
| Raw critical jobs | 4,118; zero misses/gaps | 5,161; zero misses/gaps |
| Maximum critical response | 1.149482 ms | 1.297722 ms |
| Critical deadline | 5 ms | 5 ms |
| Thermal excess in 100 ms score windows | 0.4 °C | 0.4 °C |
| Scored restoration | 3055.535 ms | 3056.604 ms |

These are separate runs of the labelled low-temperature profile. The trace-on
scorecard ends at 5,156 reported jobs; its later raw recorder ends at 5,161.
Both streams have zero internal missing records. The earlier matched-transition
campaign supplies the recorded jitter reference of 2.051 ms at n=2; this single
diagnostic capture is used to investigate the restriction path.

**[Diagnostic raw records](validation/trace-timing-evidence.zip)** ·
**[Structured measurements](validation/trace-timing-summary.json)** ·
**[Passing physical demo](low-temperature-demo.md)**

## What the delayed decision shows

Instance `low_thermal_61833269`, decision 5, epoch `44116494625222`:

| Timestamp / interval | Recorded value |
| --- | ---: |
| Detection origin | 44,127,610,630,315 ns |
| NC2 effective-off timestamp | 44,127,626,306,333 ns |
| Both-off / NC1 effective timestamp | 44,127,631,276,593 ns |
| First accepted supervisor receipt | 44,127,631,751,167 ns |
| Detection → NC2 off | 15.676018 ms |
| NC2 off → NC1 off | 4.970260 ms |
| Detection → both effectively off | 20.646278 ms |
| Detection → supervisor receipt | 21.120852 ms |

The detection/effect/receipt values are in `enforcer.txt`. The worker-effect
timestamps are retained in `runtime-samples.csv`. Receiving the acknowledgement
is measured separately from actually stopping noncritical computation.

The source has a **2 ms enforcer polling sleep** and **5 ms noncritical worker
periods**. The coordinator deliberately waits for the NC2 acknowledgement
before closing NC1. The observed 4.970260 ms second-stage interval is consistent
with waiting for a worker boundary, but does not alone establish the root cause.
The detection origin also precedes sensor read completion, so acquisition and
forwarding time belong in the end-to-end budget.

## Why this needs careful attribution

Full tracing records scheduling, IPC, kernel and interrupt activity and writes
a substantial event stream. Additional instrumentation, buffer handling, CPU0
management contention and filesystem activity are plausible contributors.
Polling phase and sequential worker acknowledgements already consume much of
the trace-off margin. A paired observation is not enough to attribute the
0.646278 ms excess solely to tracing or to one process.

The trace-off result has 1.271 ms of margin. The full-trace run preserves the
critical deadline but demonstrates that the restriction path needs more margin
under instrumentation. The target remains **20 ms**; changing its origin,
rounding away the excess, or raising the bound would not resolve this issue.

## Plan to get consistently below 20 ms

1. **Attribute each stage.** Correlate acquisition start/end, manager forwarding,
   policy decision, enforcer delivery, NC2 effect/acknowledgement, NC1 effect and
   first supervisor receipt for the same epoch and decision. Separate blocked,
   runnable and executing intervals on CPU0 from actual worker gate latency.
2. **Compare matched capture modes.** Repeat identical load, temperature bands,
   initial cooling, binary/profile and trial phases with tracing off, selected
   thread/process/user events, and full tracing. Check dropped buffers and retain
   every result. A separate focused-trace attempt did not complete its initial
   clear-condition observation; retain it as a startup diagnostic rather than
   substitute it for the complete-flow comparison.
3. **Remove avoidable polling delay.** Evaluate a bounded, nonblocking decision
   notification to wake the independent enforcer immediately. As a smaller
   experiment, compare 2 ms versus 1 ms enforcer waits and measure the added
   monitoring CPU cost. Neither improvement is claimed as measured yet.
4. **Wake cooperative gates on restrictions.** Evaluate notifying NC workers
   promptly instead of waiting for their next 5 ms release boundary. Preserve
   NC2-first ordering, leases, watchdog latching and real effect timestamps.
   Keep critical control's period, priority, affinity and deadline intact.
5. **Keep capture/logging off the urgent path.** Use selective trace classes for
   diagnosis and buffer nonessential reporting. Measure these changes before
   choosing an implementation; a cleaner trace alone does not qualify behavior
   with full tracing enabled.

Acceptance requires repeated matched threshold and expiry trials below 20 ms
for effective restriction, preserved supervisor-receipt accounting, zero critical
misses/gaps, and passing ordering/recovery/watchdog regressions. Target additional
margin below the boundary rather than a single 19.999 ms pass. Broader repetition,
jitter and total overhead qualification remain tracked in the
[implementation checklist](threadctl-checklist.md).

## Capture provenance

The full-flow trace is `profile-1795227410.kev`, produced on the QNX 8 Pi 4B
beside `/tmp/pg-low-thermal-1795226936`. Its companion records are from
`low-thermal-evidence-61833269`. The large kernel trace stays in the local/target
capture directories; the repository bundle contains the exact counters, raw
jobs, temperature/workload samples, effect confirmations and scorecard. The
trace-on record remains separate from the passing trace-off evidence.
