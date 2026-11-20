# Pi 4 thermal characterization

Build the root project with the SDP 8 environment and `make BUILD_PROFILE=release`. Copy `build/aarch64le-release/pg-thermal` to the Pi. The default sink is a local receiver in the monitor process. The optional `live` sink needs the PowerGuard service built from this branch.

## Qualify the image

1. Save the image version, Pi model, cooling configuration, ambient temperature if available, process priorities, and output of `cat /dev/thermal`. Verify units from the driver or image documentation. The reader accepts a numeric Celsius value optionally followed by C, or a numeric millidegree value with explicit `mC` selection. An unfamiliar format fails the startup probe. [QNX target image instructions](https://www.qnx.com/developers/docs/qnxeverywhere/com.qnx.doc.target_images/topic/qsti/next-steps.html)
2. Inspect `pidin -p 1 threads` and confirm the four per-CPU idle threads by name and CPU mapping on this boot (use Momentics thread details if needed). Set T0 through T3 to those thread IDs in CPU order. Verify by loading one pinned core in a separate qualification run. A different runnable thread can produce plausible but incorrect utilization. The monitor reads each idle thread's accumulated CPU time and uses 100 × (1 − idle delta / elapsed time). [QNX CPU-time clocks](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html)
3. Set UNITS to the verified unit and STOP_C to the lab's approved heater cutoff in whole degrees Celsius. Keep these settings identical across modes. The heat source is a priority-1 non-critical thread with a renewable 150 ms permit. A failed thermal read or cutoff latches it off for the run. Cooling continues to be measured. The deliberate busy-wait baseline itself consumes a core throughout the run, including idle and cooldown. Use a blocking mode for thermal characterization.
4. Confirm the sampler has permission to read the idle clocks and request high-resolution timers. Both blocking release methods request 1 ns tolerance. Scheduling latency still needs measurement. [QNX high-resolution timers](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/timing_tolerance.html)

## Capture

Use a fresh writable directory per run. Output is written after the capture to keep file logging out of each release. An interrupted capture may leave empty files. Example with the binary in /tmp and the six verified variables set:

```sh
mkdir run01 && cd run01
/tmp/pg-thermal change "$UNITS" 180 "$STOP_C" "$T0" "$T1" "$T2" "$T3"
```

This captures 60 s idle, 60 s load and 60 s cooldown at 50 ms per acquisition. Add `quiet` for an entirely unheated run. Add `40hz` for the original 25 ms design. Keep the rate, duration, CPU scheduling, sink, cooling and trace settings identical within each comparison.

| Mode | Release | Delivery |
|---|---|---|
| busy | Spin until absolute deadline | Every acquisition |
| wait | Block until absolute monotonic deadline | Every acquisition |
| pulse | Block on periodic monotonic timer pulse | Every acquisition |
| change | Same periodic timer pulse | First sample, threshold/recovery or validity change, otherwise 100 ms heartbeat |

The endpoint is read once per scheduled acquisition plus one startup probe. Timer pulses originate in the kernel. A successful file read supplies a host read timestamp and an increasing read sequence. The endpoint exposes no conversion sequence in the accepted format, so repeated values are labelled READ_ONLY internally. Failed reads carry the last successful timestamp and sequence. Missing release slots are counted and skipped.

## Acceptance and handoff

- For each rate run at least three quiet trials per mode. Rotate mode order and bring the board back into the recorded idle band before each trial. Save both CSVs, command, commit, and board settings together. A complete 60 s capture has 1,200 slots at 50 ms or 2,400 at 25 ms. Require zero missing slots, overflow, read errors and invalid CPU windows.
- Compare sampler + receiver CPU nanoseconds divided by run duration, expressed as percent of **one core**. Compare every-sample versus change mode at equal acquired counts. In a steady band expect approximately ten change-mode data pulses per second. Release pulses and data notifications are counted separately. The ≤2% steady Normal system target requires adding supervisor, manager and watchdog CPU time over the same interval. Monitor-only CPU time cannot establish that result.
- Compare p50/p95/p99/max release lateness, endpoint open/read/close, parsing and conversion, four-core counter collection, publication-to-receipt and read age. All durations are monotonic nanoseconds. The summary records clock resolution and the minimum observed back-to-back timestamp cost. CPU collection ends at load_end_ns. CSV temperature is in Celsius, the policy ABI uses tenths of a degree. The demand index is 60 for a full synthetic NC2 request and 0 when idle, under the existing 40/60 demand weights.
- Use idle/load/cooldown temperature ranges from summary.csv. Inspect samples after heat removal for additional temperature rise and time back inside the observed idle band. Flag a truncated cooldown or a heater cutoff. Select activation and lower recovery limits after comparing multiple matched runs.
- In a separate diagnostic run, start `tracelogger -s 70 -f /tmp/thermal.kev` before a 60 s capture with `trace`. Open the .kev in Momentics System Profiler. User events 81/82 bracket the thermal read plus parse and 83/84 bracket CPU collection. Match their zero-based row index to samples.csv. Inspect scheduling and blocked intervals around late samples. Use trace-off runs for scoring. [QNX tracelogger](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/t/tracelogger.html)
- For the supervisor handoff, add `live` with a single telemetry producer. The receiver submits ordered batches of at most eight samples. received_ns then means supervisor receive time. With the standalone sink it means local receiver wake time. Neither measures effective throttling. Keep manager-generated simulated inputs off this stream. The current 35 ms maximum inter-acquisition gap prevents normal recovery at 50 ms. Use 25 ms for integration and measure the observed maximum gap. The existing 2 s flat READ_ONLY rule can also conservatively flag a genuinely quiet endpoint.
- Run `pg-check` for labelled simulated policy cases including ramp, old sample replay, frozen input and freshness expiry. In target integration also stop the sole producer, record the last valid read and confirm Critical at its 150 ms age deadline, then verify both non-critical effects. For the synthetic 250 ms fault-tolerant interval record fault onset, both effective restrictions and remaining margin (250 ms minus elapsed response). This is a scenario budget, not an ISO 26262 assessment.
