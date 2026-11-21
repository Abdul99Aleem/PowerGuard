# Operator and validation scripts

| Entry point | Use |
| --- | --- |
| [jury-demo.sh](jury-demo.sh) | **Current jury demonstration.** Run beside the integrated release bundle on the QNX Pi; starts an isolated stack and captures evidence. See the [runbook](../docs/jury-runbook.md) |
| [check-host.sh](check-host.sh) | Portable C11 policy, manager-expiry, latch and Python evidence-analysis regressions; outputs under ignored `build/host-checks/` |
| [validate-threadctl.sh](validate-threadctl.sh) | QNX qualification and fault campaign; requires the full deployed bundle and verified idle-thread IDs |
| [validate-startup-latch.sh](validate-startup-latch.sh) | QNX startup admission and persistent watchdog-latch regression |
| [remote-cli/](remote-cli/) | Optional Windows SSH console helper; passes simulated policy inputs. It does not assemble or qualify the current full stack |
| [legacy/remote-cli/](legacy/remote-cli/) | Original remote-console helper retained for historical reference |
| [Legacy remote-console ZIP](legacy/PowerGuard_Remote_CLI_Windows_FIXED.zip) | Original packaged distribution, preserved unchanged; prefer the unpacked helper for inspection |

Prefer the jury wrapper for submission. Remote helper defaults describe an
earlier lab network; pass the board address and account explicitly. Never
substitute manually injected CPU values for measured CPU-load evidence.

Root `.milestone-build.py` and `.milestone-*.c/.inc` files build archived
teammate trials. Their paths remain stable for existing Momentics launch
configurations and evidence provenance. Generated executables stay ignored.
