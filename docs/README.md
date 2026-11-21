# PowerGuard documentation

Start with the [project overview](../README.md), then choose the question you
want to answer. Current submission evidence is the integrated ThreadCtl runtime
with its independent enforcer; older bench results have a different scope.

## Jury essentials

| Question | Read |
| --- | --- |
| Does it address every part of problem 16? | [Requirement-by-requirement jury checklist](jury-checklist.md) |
| How do we see it work? | [Jury demo and deployment manifest](jury-runbook.md) |
| Can we demonstrate physical temperature trips at a lower bench temperature? | [Labelled low-temperature demo](low-temperature-demo.md) |
| What did the operator's SSH rerun show? | [Independent SSH validation](ssh-validation-followup.md) |
| Which System Profiler screenshots should we take? | [Capture commands and screenshot list](system-profiler-screenshots.md) |
| What was measured, and where is the evidence? | [Current integration validation](teammate3-merge-validation.md), [raw evidence and reproduction](validation/README.md), and [JSON summary](validation/teammate3-merge-pi4-summary.json) |
| What is implemented versus hardware-qualified? | [Implementation and Pi checklist](threadctl-checklist.md) |
| Where is each function and contract? | [Source guide](source-guide.md) |

## Engineering detail

| Document | Purpose |
| --- | --- |
| [Design and manager-stall fix](powerguard-design-and-manager-stall-fix.md) | Architecture, message flow, root cause and timing definitions |
| [ThreadCtl integration](threadctl-integration.md) | QNX APIs, CPU/priority placement, build, launch and regression commands |
| [Manager-stall validation](manager-stall-fix-validation.md) | Earlier fault campaign and immutable receipt method |
| [Thermal reader](../telemetry/README.md) | Sensor format, conversion-freshness limits, acquisition and hardware characterization |
| [Real thermal integration](real-thermal-integration.md) | Single real sensor source, managed CPU load, producer freeze and target acceptance |
| [Tests](../tests/README.md) | Portable regressions and target checks |
| [Scripts](../scripts/README.md) | Current demo versus historical remote-console helpers |

## Historical references

These explain earlier stages; use the current integration report for final
claims. No historical result silently substitutes for current qualification.

- [Initial ThreadCtl Pi results](threadctl-pi-validation.md)
- [Earlier supervisor validation](supervisor-validation.md)
- [Earlier integration runbook](integration-runbook.md)
- [Teammate-3 PRD](PowerGuard-Teammate-3-PRD.docx); its historical maximum-latency
  erratum is recorded in the current integration report and JSON summary
- [Imported teammate HAM-off evidence](../tests/evidence/PowerGuard-Teammate-3-HAM-Off-Evidence.zip)

The [current raw evidence bundle](validation/pi4-integrated-evidence.zip)
contains the verified integrated run's jobs, samples, stall receipts, restart
logs and fault scorecards. Broader captures, including earlier failed attempts,
remain in ignored `results/teammate3-merge/` and the report's target paths.
