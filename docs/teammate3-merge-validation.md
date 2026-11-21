# Teammate 3 integration and Pi validation

Updated 21 November 2026. This integrates `teammate-3` commit
`5c37cb55a325f5b73382fafb95375363d0c2586a` into the ThreadCtl branch based on
`b62852e2a9a4ca4f84bbd20cdd74ed4ba820e958`. The shared ancestor is
`9647553c080437ec042571cc0d2dfe41dfb0b4f7`. The merge preserves the independent
enforcer, separate worker processes, complete critical-job ring, verified CPU
placement, and the existing 50 ms manager-progress / 20 ms receipt contract.

The committed [evidence summary](validation/teammate3-merge-pi4-summary.json)
identifies the binaries, source blobs, target directories and test scopes.
The final documentation audit also packages 26 selected verified-run raw files
in the [downloadable evidence bundle](validation/pi4-integrated-evidence.zip),
including jobs, samples, stall receipts, restart logs and both fault scorecards.
See the [reproduction guide](validation/README.md). Broader raw captures remain
under `results/teammate3-merge/` locally and the named
directories under `/tmp/pg-t3-merge-2111` on the Pi. Tests use private instance
names and stop only their own processes. HAM and physical GPIO are not enabled.

## Review problems and corrections

| Problem | Correction | Regression evidence |
| --- | --- | --- |
| A restarted manager began its local cursor at zero and resent the latest accepted job, causing EINVAL and termination | Read the supervisor's accepted source-job ID and manager sequence before forwarding; continue original source IDs, never renumber releases; a changed policy epoch resynchronizes the cursor | Ten actual manager restarts during the integrated run; no missing jobs or deadline misses |
| Latest-job snapshots could hide intermediate control releases | Retain the ThreadCtl runtime's complete job-ring protocol and batch drain; legacy snapshots explicitly expose source-ID gaps | Raw capture starts at job 1 and contains every scheduled release; scorecard also checks missing records |
| The watchdog queried the supervisor synchronously, so a reply-blocked watchdog could stop monitoring | The workload owner creates an instance-specific POSIX shared-memory progress object; the policy loop publishes lock-free monotonic progress; watchdogs read it directly and use nonblocking pulses | Supervisor is SIGSTOP'ed for one second; independent watchdog detection and gate closure occur while it is stopped |
| A normal newer grant could undo watchdog forced-off | First watchdog trip stores an immutable timestamp; runtime and legacy enforcement reject every positive grant thereafter; policy reports FORCED_OFF | Workloads remain off after supervisor resume; unit checks also try positive caps after a newer epoch publication |
| A failed critical-job or B/C CPU-budget check did not fail the overhead suite | One shared qualification predicate returns nonzero for invalid windows, missed jobs and B/C overhead above 2%; mode A remains an intentionally busy reference but must still meet critical deadlines | Unit negative cases and actual B/C/A smoke measurements |
| The benchmark builder failed with retained ThreadCtl code | Build current root prerequisites; use GNU C11 consistently; add include paths and ThreadCtl/progress objects; guard _QNX_SOURCE; generate embedded assets before HAM-suite compilation | Default, handoff, overhead and HAM-suite cross-builds |

The legacy worker improvements are retained: 500 us maximum wall-clock compute
chunks, monotonic condition-variable wake-ups for cap changes, and a faster
poll while a Critical effect is pending. These are cooperative workload gates.
The integrated runtime continues to use its separate critical, NC1 and NC2
processes rather than treating the legacy benchmark as the production runtime.

## Watchdog and run lifecycle

The workload owner acquires its unique channel before creating
`/powerguard_progress[_PG_INSTANCE]` with `shm_open()`, `ftruncate()` and `mmap()`.
The mapping contains version, policy epoch, progress time and forced-off time.
Initialization verifies that the shared 64-bit atomics are lock-free. A stale
object may be replaced only by the process that acquired the workload channel;
existing mappings remain attached to the previous run.

Start workloads before the supervisor. The supervisor opens that run's mapping,
publishes progress on every receive-loop pass, and schedules a wake at least
every 100 ms even in quiet operation. No device read, manager acknowledgement,
CLI rendering or allocation is added to the policy loop.

The watchdog checks every 50 ms. At 200 ms without progress it atomically
latches forced-off and wakes the workload endpoint without waiting for a reply.
The integrated runtime still requires the watchdog's heartbeat within 100 ms;
worker/enforcer health and absolute grant expiry remain independent checks.
The initial supervisor-start allowance is 5 s, during which noncritical gates
remain unqualified. Failure to establish progress trips the latch.

There is no live latch-clear operation. Fresh samples, a resumed supervisor,
new decision IDs, and restarting only the manager cannot reopen gates. Restart
the complete workload/runtime, supervisor, manager, enforcer and watchdog as a
new run. The owner releases its named mapping at normal shutdown.

`pg-enforcer` remains the only process authorized to deliver grants to the
integrated runtime. It still reports causal effects independently of the
telemetry manager. Transient supervisor timeout/interruption retries withdraw
qualification through the existing leases; permanent protocol/runtime errors
remain failures. Both named services also complete pending client-unblock
requests. The watchdog does not rely on this timeout handling to monitor the
supervisor.

`PG_VERSION` request/reply sizes are unchanged. The existing reserved uint32_t
in benchmark protocol v4 now reports `forced_off`; no field is inserted into
the wire layout. The additive policy cause is `FORCED_OFF`. Rebuild the full
binary set together and do not mix the archived protocol-v2 benchmark binaries
with the current protocol-v4 runtime.

## Fresh integrated Pi results

Target: `10.78.56.80`, Raspberry Pi 4B, QNX 8.0.0, four CPUs. The final release
build uses the existing CPU1/FIFO30 critical controller, CPU2/FIFO10 NC1,
CPU3/FIFO8 NC2, and CPU0 management processes. Temperature is simulated;
per-core utilization is measured from qualified idle clocks over the declared
100 ms sliding window. Critical work remains the fixed-size 100,000-iteration
calculation with a 10 ms period and 5 ms relative deadline.

| Check | Measured result |
| --- | --- |
| Manager SIGSTOP, 30 one-second trials | 30/30 supervisor receipts before manager resume; worst expiry-to-receipt 12.135167 ms, against 20 ms |
| Manager progress origin | Timeout remains 50 ms; worst pause-to-receipt 61.314777 ms, which includes the timeout |
| Restrictions before deliberately stopping the supervisor | 36 confirmed, zero late/unconfirmed; worst effect 13.820500 ms and receipt 14.798204 ms |
| Critical jobs across the entire raw run, including supervisor stop | 21601 records; zero missing IDs, period errors or deadline misses; maximum response 1.148518 ms |
| Manager restarts | Ten restarts; original job IDs and accepted progress sequence continue without gaps |
| Supervisor stall and persistent latch | Both-off/latch observed 235.188630 ms after last policy progress, within 270 ms; still off after resume |
| Controlled restoration before the deliberate latch | 35 restores; worst 3057.061 ms, including the declared 2 s + 1 s observation |
| Sampled overshoot | 0.0 C simulated thermal, 6 CPU percentage points and 2 demand-index points in the existing 100 ms score windows |
| Observed Normal-to-Warning timing spread | 2.051 ms at n=2; phase matching and full jitter qualification remain pending |

The raw archive is contiguous through ID 21,601. The post-B5 scorecard reads
21,602 reported jobs because it ends at a later instant. Each stream reports
zero internal missing records; these are separate capture endpoints.

The supervisor-stop case has a different oracle from the manager-stop case.
While the supervisor itself is paused it cannot accept an effect receipt. The
combined post-B5 score therefore retains 1 late restriction with
worst 959.400 ms. This is archived as a deliberate fault result, not reported
as a passing 20 ms confirmation score. B5 is evaluated using independent
progress/latch/worker evidence against 270 ms. The pre-B5 score and the 30
manager-stall receipts retain their separate, passing 20 ms scopes.

A final hardware run also exposed a manager recovery race: when SIGSTOP landed
before a policy receive, the manager's send timed out and it exited. This did
not defeat worker closure or the persistent latch, but it failed the process
survival assertion. The manager now backs off on transient sample/job/progress
IPC timeouts, then rereads accepted job and progress cursors before replaying.
This handles a request that might have been accepted before its reply was lost.
The failed capture is retained and the complete final regression is repeated
after the correction. Permanent errors, missing records and restarted workload
IDs remain failures rather than silent cursor resets.

## Legacy benchmark results and evidence corrections

The handoff test and B/C/A quiet smoke comparison run separately from the
integrated stack. They use private instance names, simulated temperature and
the legacy single-process workers. Each smoke window is approximately 2 s,
with one run per mode and 80 replay acquisitions. Observed supervision costs
are B **0.47866%**, C **0.45279%**, A **99.91499%** of one core. C sends 20 notices
against B's 80, a 75% reduction. Every smoke window has zero critical misses
and gaps. These short windows do not establish a CPU saving of C versus B or
qualify the separate-process stack's total supervision cost.

The new legacy watchdog-fault check also stops the actual supervisor and
checks persistent off after resume. Its gate timestamp uses the legacy
aggregate worker acknowledgement; per-worker fields are available only in the
split runtime. The first version of the cross-runtime probe incorrectly
required those absent per-worker fields, reported `both_off_ns=0`, and failed.
That failed capture is retained, the probe is corrected, and both variants are
rerun with the corrected timestamp contract.

A separate cold-start negative test leaves the legacy bench without any policy
epoch, waits for the 5 s qualification allowance to trip, then attempts manual
positive admission. The request is rejected and both caps remain zero. Both
admission and every worker boundary enforce the latch, including the legacy
manual-level endpoint. This closes a bypass that ordinary versioned-grant tests
alone would not cover.

The imported DOCX and ZIP remain historical artifacts. The ZIP's final trace
and console identify **1.877203 ms**, not the DOCX's 1.740 ms, as the final
1,000-release window maximum. The complete archived raw file contains 1,334
records with zero misses/gaps. Its final supervision subtotal is 0.5262%; the
DOCX's 0.5575% describes an earlier measurement and must not be substituted
for that final run. The historical nine-window overhead summaries report
54,000 releases, but their complete per-job files are not bundled. These
archived measurements are not substituted for current-build validation.

## Reproduction

In a QNX SDP 8 environment, build and stage in a new private target directory:

```sh
make BUILD_PROFILE=release CCFLAGS='-Wextra -Werror'
make -C testbench
python .milestone-build.py --handoff
python .milestone-build.py --overhead
```

The milestone builder rebuilds its root prerequisites. Set
`PG_BUILD_PROFILE=release` when embedding release rather than debug root assets.
Its workload instrumentation is O2 in either mode; record the selected profile.

Beside the rebuilt release binaries and the protocol-v4 pgbench CLI on the Pi:

```sh
PG_TRIALS=2 PG_STALL_TRIALS=30 PG_RESTART_TRIALS=10 \
  sh validate-threadctl.sh 1 2 3 4
```

The four arguments must be qualified idle TIDs for that image, not guessed on
another BSP. `pg-threadctl-check`, `pg-check`, `pg-manager-check` and
`pg-merge-check` run first. B5 runs last because restoring capacity after that
test requires a new run. Capture pre-B5 and post-B5 scorecards separately.

In a separate quiet interval, with the embedded benchmark executables staged:

```sh
PG_INSTANCE=t3_handoff ./pg-t3-handoff
PG_INSTANCE=t3_overhead ./pg-t3-overhead --smoke
PG_INSTANCE=t3_watchdog ./pg-t3-overhead --watchdog
sh validate-startup-latch.sh
```

Omit `--smoke` for the original rotating ABC/BCA/CAB comparison: three 60 s
windows per mode after 30 s warm-ups. Mode A exceeding 2% is expected; any
critical-job failure still fails the suite. B/C exceeding 2% fails it.
Building `--ham-suite` is supported; activating HAM and testing automatic
restarts remain separate optional work and were not performed here.

## Remaining delivery qualification

- Repeat the full nine-window overhead series with the corrected gates and
  archive every per-job trace; measure the complete split stack including its
  enforcer and actual sensor cost separately.
- Run a larger matched-phase transition series and qualify the PRD jitter bound.
- Integrate the real thermal producer, conversion-freshness evidence and
  post-restriction physical peak/cooldown measurements.
- Verify all five physical LED indications against CLI state.
- Qualify worker/coordinator/enforcer death and restart, permission-denied
  startup, representative interrupt load, long soak, and Pi 5 separately.
- Retain the distinction between affinity, workload gates, estimated demand
  index and measured physical power. Hardware power/frequency control is stretch work.
