# Evidence and final audit

Latest additions:

- [PG-PERF-01 timing investigation](../timing-investigation.md): exact full-trace
  restriction timestamps, preserved critical timing, and a staged optimization
  plan with separate diagnostic evidence.

- [Operator's SSH rerun](../ssh-validation-followup.md): 1,923 complete critical
  jobs, zero misses/gaps, standard temperature profile.
- [Physical low-temperature demo](../low-temperature-demo.md): 1,564 real
  readings, all five states, 4,118 complete jobs with zero misses, and an actual
  temperature-only trip under separately labelled demo limits.
- [System Profiler screenshots](../system-profiler-screenshots.md): exact capture
  commands and the five recommended views.

## Real sensor follow-up

The [real thermal evidence bundle](real-thermal-pi4-evidence.zip) and
[summary](real-thermal-pi4-summary.json) record the new verified integration:
730 actual sensor readings, real managed CPU load, producer-stop fail-safe,
staged recovery, and 2,135 complete critical jobs with zero misses. Read the
[live report](../real-thermal-integration.md) for scope, the retained initial
capture-shutdown failure, and the subsequent passing full run.

Extract this bundle and run `tests/analyze_threadctl.py` on `final/jobs.csv`;
the expected maximum response is 986,500 ns. The bundle also includes a short
default-mode compatibility regression under `simulated-regression/`, both its
pre/post-watchdog scorecards, native check output and release binary hashes.
The older evidence below retains its original, separately measured scope.

## Inspect the integrated Pi run

**[Download pi4-integrated-evidence.zip](pi4-integrated-evidence.zip)** (about
428 KiB). This packages existing verified-run captures from
`results/teammate3-merge/verified-core/`; it does not represent a new board run.
The target was Raspberry Pi 4B, QNX 8.0.0, aarch64le release.

| Files | What they establish |
| --- | --- |
| `jobs.csv` | All 21,601 captured critical releases, start/finish times and deadlines |
| `samples.csv` | Simulated temperature, measured per-core CPU, caps, states and freshness |
| `stall-confirmations.txt` | Thirty unique manager-stall decisions with immutable detection/effect/receipt timestamps |
| `scores.txt` | Pre-supervisor-stall five-metric scorecard |
| `latched-scores.txt` | Deliberately retained post-supervisor-stall late receipt |
| `supervisor-latch.txt`, `latched-status.txt` | Independent 270 ms watchdog test and retained forced-off state |
| `manager-restart-0.txt` through `manager-restart-9.txt` | Ten actual manager restart logs; job continuity is checked against the raw job stream |
| `assertions.txt`, `scenarios.txt`, readiness and check logs | Scenario assertions, placement and regression context |
| `manifest.json` | SHA-256 for each of the 26 included raw files and source-directory provenance |

The bundle is a selected verified-run evidence set. It does not contain the
full earlier failure campaign, QNX binaries, or physical LED/thermal evidence.
The [integration report](../teammate3-merge-validation.md) retains the wider
provenance, corrected failures and known limitations. The [JSON summary](teammate3-merge-pi4-summary.json)
also identifies the tested release binaries and source blobs.

## Reproduce the numbers without QNX

From the repository root with Python 3, extract into an ignored output directory:

```sh
python -m zipfile -e docs/validation/pi4-integrated-evidence.zip build/pi4-evidence
python tests/analyze_threadctl.py build/pi4-evidence/jobs.csv
python tests/analyze_manager_stalls.py build/pi4-evidence/stall-confirmations.txt
```

Both analyzers must report `"pass": true` and exit successfully. Job analysis
must show 21,601 jobs, zero gaps/misses/period errors and maximum response
1,148,518 ns. Stall analysis must show 30 unique decisions, maximum receipt
12,135,167 ns from the 50 ms progress expiry and no errors. These exact fields
were compared against the committed summary during packaging.

Raw recorder and scorecard endpoints differ: the raw stream ends at job 21,601;
the post-fault scorecard reports 21,602. Both have zero internal missing records.
The post-fault late restriction remains a failure of the receipt metric even
though the separately measured watchdog latch passes.

## Final repository audit · 21 November 2026

- Reviewed README claims, policy constants, worker placement, watchdog/receipt
  contracts and documentation against problem 16.
- Replaced the planned-directory map with actual modules; added a jury
  requirements matrix and documentation/script indexes.
- Grouped optional remote helpers into `scripts/remote-cli/` and historical
  versions into `scripts/legacy/`; updated source-guide links. Existing runtime,
  Momentics project paths and benchmark build paths remain stable.
- Fresh QNX SDP 8 aarch64le **debug and release builds passed**, with
  `-Wall -Wextra -Werror` for C sources, in separate ignored audit directories.
  QCC emits its existing notice that `-pthread` is unnecessary on QNX.
- Forced standalone testbench/HAM-helper rebuild passed with the same warning
  flags. This verifies compilation, not optional HAM activation.
- Portable C11 policy/metrics replay, manager progress/receipt checks and
  watchdog-latch/negative benchmark checks passed using host Clang.
- All four Python evidence-analysis regression tests passed. Archived jobs and
  stall receipts reproduced every summary field except the relocated source path.
- No new Pi timing, physical LEDs, integrated real-temperature loop, Pi 5,
  full overhead or long soak qualification is claimed by this audit. See the
  [open qualification checklist](../threadctl-checklist.md).
