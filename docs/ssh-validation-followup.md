# Independent SSH validation · 21 November 2026

The operator repeated the real-input acceptance script through SSH:

```sh
cd /tmp/pg-real-thermal-1795224989
sh validate-real-thermal.sh C 1 2 3 4
```

Instance: `real_thermal_55328819`. The raw directory was retrieved from the Pi
and the critical-job analyzer was rerun independently. This run is separate
from the earlier 2,135-job acceptance capture.

| Check | Operator SSH output and retrieved evidence |
| --- | --- |
| Temperature source | REAL /dev/thermal, READ_ONLY quality, 25 ms pulse releases |
| Physical acquisitions | 635 readings, 54.0–57.4 °C; zero read errors, overflow or invalid CPU windows |
| Normal | NC1 100%, NC2 100%; fresh real temperature |
| Managed CPU spike | Throttled, NC1 100%, NC2 0% |
| Temperature producer paused | Critical, both NC workloads 0%; sensor age 1109.260 ms at the displayed snapshot, cause `0x8` |
| Fresh readings resumed | Controlled recovery to Normal, both NC workloads 100% |
| Restriction response | 2 confirmed, zero late/unconfirmed; worst effective restriction 9.469 ms against 20 ms |
| Critical timing, displayed scorecard | 0 misses / 1,912 reported jobs; maximum displayed response 1.137 ms |
| Critical timing, complete downloaded CSV | 1,923 jobs, zero misses/gaps/period errors; maximum response **1.137481 ms** against 5 ms |
| Workload overshoot | CPU 4 percentage points, demand index 2 points in 100 ms score windows |
| Restoration | 2 recoveries; worst 3052.770 ms including 2 s + 1 s dwell |
| Transition jitter | No matching trials in this run (`n=0`); not qualified here |

The raw recorder ends later than the scorecard, explaining 1,923 versus 1,912
jobs. Both streams have zero internal missing records. The 120 s sensor
allocation was deliberately stopped after acceptance (`interrupted=1`);
`missing=4165` includes its unused allocation and the deliberate producer pause.

**[Download this run's raw evidence](validation/ssh-real-thermal-evidence.zip)** ·
**[Machine-readable summary](validation/ssh-real-thermal-summary.json)**

Extract the ZIP and run:

```sh
python tests/analyze_threadctl.py /path/to/extracted/jobs.csv
```

## Why Critical appeared at 56.9 °C

The standard thermal activation thresholds are **65 °C Warning, 70 °C
Throttled, 75 °C Critical**, with recovery at **62 °C or below**. Neither 52 °C
nor 56.9 °C is a standard Critical thermal limit. The displayed `causes=0x8`
means **BAD_TEMP**: the producer was deliberately paused and its original
acquisition timestamp expired after 150 ms. The displayed age is a later
snapshot, not the instant at which restriction started.

The system also responds to CPU, estimated demand, manager progress and
watchdog faults. A temperature by itself does not uniquely determine state;
previous restrictions and recovery dwell also matter.

For an actual physical-temperature threshold crossing at ordinary bench
temperatures, use the separately labelled
[low-temperature demonstration](low-temperature-demo.md). Its limits are demo
settings, not revised production thermal limits.
