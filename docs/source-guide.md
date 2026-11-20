# PowerGuard source guide

Team Schedulix · QNX eHACK submission

PowerGuard runs one periodic critical workload and two budgeted noncritical workloads on a four-core QNX target. To follow a restriction from input to evidence, start with `pg_ingest` in the policy, then read `main` in the enforcer, `ordered_cap` in the runtime and `pg_effect` back in the policy. A decision, a worker applying it, and the supervisor receiving confirmation each have a separate timestamp.

[Supervisor and policy](#supervisor-and-policy) · [Interfaces](#interfaces) · [Runtime and platform](#runtime-and-platform) · [Telemetry and operator tools](#telemetry-and-operator-tools) · [Bench and target trials](#bench-and-target-trials) · [Regression checks](#regression-checks) · [Build and operator scripts](#build-and-operator-scripts)

## Supervisor and policy

The supervisor starts in Critical with both noncritical budgets at zero. For valid inputs the highest active band governs escalation. The table gives the configured comparison values. The clear boundary is inclusive downward, and the three activation boundaries are inclusive upward.

| Input | Clear | Warning | Throttled | Critical |
| --- | ---: | ---: | ---: | ---: |
| Temperature (°C) | 62 | 65 | 70 | 75 |
| Busiest core (%) | 55 | 70 | 85 | 95 |
| Power demand index (0–100) | 45 | 60 | 75 | 90 |

The default index is the integer-weighted sum of requested NC1 and NC2 loads, with weights of 40% and 60%. The policy also accepts an explicit index. Critical closes both budgets. Throttled closes NC2. Restoration requires acknowledged caps and fresh, continuously clear measurements. After recovery from Critical, NC1 may reopen after two seconds of clear input and NC2 after a further second. The protected critical worker never receives an NC cap.

### [src/PowerGuard.c](../src/PowerGuard.c)

The `PowerGuard` executable owns one policy and one metrics accumulator per service instance.

- `monotonic_ns` reads the local monotonic clock for policy deadlines and receipts.
- `trace_change` emits QNX trace event 80 when the decision, state, causes, effective time or clear interval changes.
- `main` checks FIFO priority and placement, attaches the named channel, publishes supervisor progress and arms the next expiry. Its receive loop handles timer and disconnect pulses, validates incoming requests, ingests samples or batches, accepts worker effects and manager progress, and replies with status, scores or the first accepted receipt.

### [supervisor/policy.c](../supervisor/policy.c)

The five-state policy runs on caller-supplied monotonic timestamps. The same implementation drives the target service and the host replay.

- `severity` maps a measured value to its band. `pg_activation_limit` returns the crossed threshold for an overshoot measurement. `pg_clear_values` checks all three clear thresholds. `pg_recovery_ready` also requires an empty cause mask.
- `set_stage` writes NC1/NC2 caps, advances the decision ID and resets effect and dwell timestamps. `advance_fsm` applies escalating restrictions and advances recovery when fresh evidence satisfies dwell. An interrupted recovery returns to its entry restriction.
- `pg_init` establishes the epoch and the initial zero-cap decision. `fresh` tests a nonzero timestamp against the clock and an exclusive age limit. `earlier` selects the earlier of two detection times.
- `ingest` validates acquisition identity and load timestamps, accounts for expiry between arrivals, and stores the sample. `pg_ingest` uses weighted NC demand. `pg_ingest_power` selects the caller's power index for that sample.
- `evaluate` combines sensor quality, peak CPU, demand, lease expiry, manager progress and the watchdog latch. It feeds the resulting severity into the state machine and records the detection time of a tighter cap.
- `pg_tick` reevaluates the stored inputs at a new time. `pg_next_deadline` selects the next pending expiry for the QNX timer.
- `pg_effect` validates the epoch, decision, cap pair and worker timestamp. It preserves the supervisor's first receipt for the decision. `pg_manager_progress` accepts fresh, increasing manager sequences and records a missed progress deadline even if the next update arrives after expiry.

### [supervisor/metrics.c](../supervisor/metrics.c)

The scorekeeper consumes policy snapshots and critical-job reports. Restriction scoring begins after the first Normal state.

- `pg_trial` arms a timed transition for a case and a specific pair of states. A new trial supersedes an unfinished one.
- `pg_job` validates job order and timestamps, counts gaps and deadline misses, and records response time. A zero finish can represent an overdue, unfinished job.
- `pg_observe` compares successive snapshots. It tracks effective-restriction latency, unconfirmed decisions, transition timing spread, 100 ms overshoot windows and time to acknowledged restoration. `max_time` updates the recorded maxima.

## Interfaces

All policy and IPC timestamps use local monotonic nanoseconds. The bench reports temperatures in millidegrees Celsius. The policy accepts tenths of a degree. The manager converts the units at the boundary.

### [include/policy.h](../include/policy.h)

Defines the five states, cause bits, source and quality tags, sample, status and policy state. The API exposes `pg_init`, `pg_ingest`, `pg_ingest_power`, `pg_tick`, `pg_next_deadline`, `pg_effect` and `pg_manager_progress`. The threshold queries are `pg_activation_limit`, `pg_clear_values` and `pg_recovery_ready`. The timing constants include 150 ms input ages, a 50 ms manager progress limit and a 20 ms response target.

### [include/ipc.h](../include/ipc.h)

Defines the supervisor's version 1 requests and replies, including batch samples, relative job budgets, manager progress, score queries and effect confirmations. A batch holds up to eight samples. The confirmation reply carries detection time, first accepted receipt time and manager progress alongside the decision snapshot.

### [include/metrics.h](../include/metrics.h)

Defines the public score counters, per-case transition spreads and the accumulator's pending decisions and overshoot windows. Declares `pg_trial`, `pg_job` and `pg_observe`, implemented in the supervisor metrics module.

### [include/bridge.h](../include/bridge.h)

Adapts the two IPC vocabularies. `pg_job_budget_ns` converts an absolute job deadline into a release-relative duration and returns zero for an invalid interval. `pg_bench_state_from_policy` accounts for the bench's reserved Unknown state when publishing a policy state.

### [include/pg_names.h](../include/pg_names.h)

`pg_instance_name` validates the optional `PG_INSTANCE` suffix and constructs a local service or shared-memory name. `pg_policy_name` and `pg_bench_name` supply the supervisor and workload endpoint names to their clients.

### [include/pg_progress.h](../include/pg_progress.h)

Defines the shared progress record, the 200 ms freshness limit and the watchdog pulse codes. `pg_progress_open` and `pg_progress_close` manage its mapping. `pg_progress_publish` publishes the epoch and timestamp. `pg_progress_stale` checks the age of the last published progress. `pg_progress_trip` atomically stores the first forced-off time, `pg_progress_forced` reads it, and `pg_progress_allows` rejects nonzero caps once it is set.

### [include/pg_threadctl.h](../include/pg_threadctl.h)

Defines a placement report with PID, TID, CPU, priority, scheduling policy, masks and thread name. Declares the platform operations `pg_thread_cpu_count`, `pg_thread_masks_get`, `pg_thread_masks_set`, `pg_thread_configure`, `pg_thread_verify` and `pg_thread_print`.

### [include/pg_qualification.h](../include/pg_qualification.h)

`pg_overhead_result` returns 2 for an invalid trial, 1 for job errors or a breached CPU budget, and 0 for a pass. The two sleeping acquisition modes have a 2% single-core budget. The busy reference mode still requires complete critical jobs.

### [testbench/pgbench_protocol.h](../testbench/pgbench_protocol.h)

Defines version 4 of the local workload IPC contract. Its commands cover status, scenarios, state reporting, caps, leases, jobs, watchdog and enforcer presence. The status reply carries both requested and applied levels, worker identities, freshness, timing and job loss. `pg_job_record.deadline_ns` is absolute. A job-stream reply holds up to 16 releases.

### [runtime/protocol.h](../runtime/protocol.h)

Defines the private coordinator/worker exchange and the `pg_worker_main` entry point. A worker reports its role, placement, applied decision and job. The coordinator replies with a cap, demand and expiry. The critical release period is 10 ms with a 5 ms deadline. Noncritical workers run on a 5 ms period.

## Runtime and platform

The split runtime places the coordinator on CPU0 and the critical, NC1 and NC2 workers on CPUs 1, 2 and 3 respectively. The manager forwards data and job records. A separate enforcer delivers leases and returns paired worker acknowledgements, so a stalled manager does not have to report its own failure.

### [manager/pg-manager.c](../manager/pg-manager.c)

Bridges workload and supervisor IPC for both the threaded bench and the split runtime.

- `timed_send` bounds the send and reply phases. `bench_send` builds a workload command and propagates its error. `policy_send` issues a versioned supervisor exchange.
- `resume_cursor` reads the accepted job ID, progress sequence and epoch from two supervisor replies. It checks the epochs agree before resuming. `retry_policy` identifies interrupt and timeout cases eligible for resynchronization.
- `clamp_percent` bounds the legacy process-CPU estimate to 0–100. `stop` signals the polling loop to finish.
- `main` qualifies placement, converts bench telemetry, forwards critical-job records and publishes manager progress for the split runtime. With the threaded bench it also issues leased caps and returns paired effects. Instrumented trials can record CPU crossings and confirmation times.

### [runtime/enforcer.c](../runtime/enforcer.c)

`main` verifies its own FIFO placement, refreshes the coordinator heartbeat, reads the policy and delivers each cap pair with its lease. It submits an effect only after the applied epoch, ID and caps match, then logs the supervisor receipt. `send_timed` bounds an exchange to 2 ms. `pause_tick` paces retries and polls. `stop` ends the loop on a termination signal.

### [runtime/runtime.c](../runtime/runtime.c)

The coordinator owns worker processes, grants, the job ring and the live workload status endpoint.

- `ns_clock` reads a specified clock, `now` reads monotonic time, and `send_timed` bounds a client exchange. `parse_positive` checks numeric arguments. `stop` requests shutdown.
- `idle_setup` checks four distinct procnto idle threads against their CPU, name and priority before opening their accounting clocks. `tick` builds per-core busy windows, refreshes simulated thermal data, checks worker and watchdog health, and detects exited children.
- `observe_force_off` applies the shared watchdog latch to the coordinator's grant state. `gates_allowed` checks readiness, watchdog, enforcer and lease freshness. `ordered_cap` sequences NC2 closure before NC1 and NC1 reopening before NC2.
- `worker_report` validates the sender and placement, records jobs or worker acknowledgements, and replies with the current grant. `caps` validates incoming leased decisions and controls renewal or replacement.
- `serve` starts the children, owns the QNX channel and dispatches pulses, worker reports and public workload requests. `client` implements status, job dump/recording, samples, watchdog and stop commands. `main` selects coordinator, worker or client operation from the arguments.

### [runtime/worker.c](../runtime/worker.c)

`pg_worker_main` runs one role in its own process. The critical role reports fixed-work jobs with release, start and finish times. Each NC role verifies placement, reports the cap it applied and bounds further work by grant expiry. `compute` executes the synthetic work loop, `monotonic_ns` reads the clock, `sleep_until` waits for an absolute release with high-resolution tolerance, and `stop` requests an orderly exit.

### [platform/progress.c](../platform/progress.c)

`progress_name` resolves the instance's shared-memory name. `pg_progress_open` creates or joins the mapping, checks its size and lock-free atomic support, and initializes or verifies its version. The process that owns the workload endpoint creates the mapping for a new run. `pg_progress_close` unmaps it and lets the owner unlink the name.

### [platform/threadctl.c](../platform/threadctl.c)

Provides the common QNX scheduling and placement checks.

- `pg_thread_cpu_count` reads the system-page CPU count. `exchange_masks` packs and unpacks the variable-width mask words for `ThreadCtl`. `pg_thread_masks_get` reads both masks. `pg_thread_masks_set` sets them and checks the readback.
- `thread_name` sets or reads the current thread name. `pg_thread_verify` cross-checks the kernel's mask APIs and captures scheduling and identity. `pg_thread_configure` applies FIFO priority, run and inherit masks, and name, then verifies each field. `pg_thread_print` emits the placement record consumed by the target scripts.

## Telemetry and operator tools

### [telemetry/thermal.c](../telemetry/thermal.c)

The `pg-thermal` program samples the board's `/dev/thermal` device and four idle clocks. It can forward batches to the supervisor and writes acquisition and timing evidence in a fresh run directory.

- `read_raw` reads one bounded text sample and handles interrupted reads. `parse_temperature` validates the configured Celsius or millidegree scale and the allowed range, then rounds to deci-degrees.
- `ns` reads a clock in nanoseconds. `idle_time` returns an invalid sentinel on an idle-clock read failure. `ns_to_timespec` prepares timer values. `die` reports a fatal system error. `parse_integer` validates numeric arguments.
- `connect_local` attaches to a private channel. `mark` emits optional trace markers. `receiver` drains published ring entries on pulses, optionally sends a batch, timestamps delivery and reports an overflow fault at shutdown.
- `heater` runs low-priority synthetic work during commanded heat intervals. `compare_ns` orders latency values for `write_distribution`, which writes median, upper percentiles and maximum.
- `main` qualifies the sensor and scheduling, selects busy, wait, pulse or change-driven acquisition, starts the threads, records samples and writes the run summary.

### [gpio/pg-state-leds.c](../gpio/pg-state-leds.c)

`main` selects the Pi 4 or Pi 5 GPIO utility and polls the policy every 100 ms. Normal, Warning, Throttled, Critical and Recovery map to pins 17, 27, 22, 23 and 24. `read_state` validates a timed supervisor reply. `set_gpio` launches the driver and checks its exit status. `stop` ends polling so the program can turn the pins off.

### [tools/pg-console.c](../tools/pg-console.c)

The console runs against an in-process simulation or the live supervisor. Its input commands can supply simulated readings. Its score command reads the accumulated measurements.

- `integer` validates bounded integers, `state_id` resolves a state name, and `ms` converts nanoseconds for display.
- `target_now` and `send_qnx` supply target-side monotonic time and timed supervisor IPC. `snapshot` copies the local policy state and last sample into the simulated reply.
- `frame` handles one sample interval. Live mode sends the input and power index to the service. Local mode advances the policy, simulates an effect acknowledgement and updates metrics. `readings` parses temperature, CPU, power, quality and duration and drives the requested frames.
- `display` prints the resulting transition and input details. `report` prints the five scores and the selected transition series. `command` dispatches trials, scores, synthetic jobs and readings. `line` tokenizes interactive input. `main` selects live, interactive, one-shot or built-in example operation.

### [tools/pg-validate.c](../tools/pg-validate.c)

`main` reads runtime and policy snapshots, verifies worker and enforcer placement, readiness and applied caps, and checks the expected state or closed condition. `send` bounds the request/reply exchange. The result is suitable for a target script's exit-status check.

### [tools/pg-stall-check.c](../tools/pg-stall-check.c)

`main` pauses the specified manager, waits for an independently confirmed restriction, checks the receipt against progress expiry, and resumes the process. `read_receipt` queries the supervisor's stored confirmation. `now` timestamps observations, `pause_ms` spaces probes and `interrupted` resumes the manager before exiting on a signal.

### [tools/pg-supervisor-check.c](../tools/pg-supervisor-check.c)

`main` pauses the supervisor and observes the shared watchdog trip and effective worker caps while the supervisor is stopped. It resumes the supervisor and checks the result. `send` queries the workload endpoint, `now` timestamps observations, `pause_ms` spaces probes and `interrupted` resumes the supervisor before exiting on a signal.

## Bench and target trials

### [testbench/pgbench.c](../testbench/pgbench.c)

The threaded workload server shares the public bench protocol with the separate-process runtime. It also provides the `pgbench` command-line client used to select scenarios.

- `now_ns`, `sleep_until` and `burn_until` read a clock, sleep to an absolute deadline and consume CPU until a deadline. `init_cap_condition` sets up monotonic condition-variable waits for NC cap changes.
- `critical_worker` runs periodic work and records release, response, jitter and missed jobs. `noncritical_worker` executes the requested duty cycle within the current cap and lease. `worker_cap` resolves expiry, forced closure and NC ordering at a worker boundary, then records the applied acknowledgement.
- `ramp_temperature` computes the simulated rising temperature and records threshold times in the instrumented fast-ramp trial. `sampler` updates simulated temperature, age and process CPU load. In the overhead build it returns immediately because the separate monitor supplies the samples.
- `snapshot` takes a synchronized copy of status. `write_csv` serializes one status row. `csv_writer` samples those rows for the run log. `sig_stop` asks the service to finish.
- `apply_caps` checks cap values, decision identity and lease validity before updating the workers' request. `serve` creates the QNX endpoint, starts the threads, dispatches commands and joins the threads on shutdown.
- `print_status` formats the operator snapshot. `dashboard_send` issues a dashboard request. `dashboard` runs the interactive menu. `usage` lists valid invocations and `main` selects server, dashboard or one-shot client operation.
- In trial builds, `record_job` and `record_effect` append to bounded evidence buffers. `export_jobs` writes the release/start/finish CSV. `export_effects` writes worker acknowledgements and any captured lease-expiry and fast-ramp data. `pin_spike_thread` applies the requested CPU placement for the measured spike experiment.

### [testbench/pgbench_ham.c](../testbench/pgbench_ham.c)

`main` connects to the QNX High Availability Manager, attaches the named entity and registers a restart-on-death action for the supplied absolute command.

### [.milestone-runner.c](../.milestone-runner.c)

The on-board harness stages embedded binaries into a private run directory. Each build selects its trial at compile time. The runners use the recorded worker and supervisor timestamps to score results.

- `stage` exclusively creates an executable from embedded bytes. `launch` spawns a child, `reap` bounds the wait for its exit and `stop` shuts down the trial stack. `pause_ms` handles interrupted waits and `monotonic_ns` supplies trial timestamps.
- `request` opens a named service and bounds an IPC transaction. `read_policy` requests its decision snapshot. `bench_command` sends a workload command. `read_stack` collects both snapshots for one observation.
- `check_jobs` audits release spacing, timestamps and deadlines inside the scored interval. `check_worker_order` matches both zero-cap acknowledgements to the tested decision. `check_ftti_timing` relates the simulated fast temperature ramp to issuance, worker shutdown and confirmation.
- `wait_for_gated_clear` finds evidence that recovery dwell started with both caps closed. `run_recovery` interrupts and restarts that dwell, then observes NC1 followed by NC2. `check_recovery_effects` checks the actual opening timestamps for those stages.
- `run_worker_fault` exercises delayed or absent NC2 acknowledgement. `check_fault_effects` checks the worker log for that fault mode. `run_manager_loss` stops and restarts the manager. `check_lease_expiry` compares local lease expiry to the gates closing.
- `start_cli` starts the interactive client with a pipe and captured output. `send_cli_line` writes a command and `cli_log_size` checks captured output length. `pause_cli` measures a stopped-client interval, `close_cli` verifies a clean client exit, and `run_cli_trial` checks supervisor and worker operation around both events.
- `run_spike_trial` requests the NC2 spike and waits for matching throttling. `check_spike_effect` correlates the measured per-core sample with the affected worker's acknowledgement.
- `run_trial` performs preflight, starts the stack, establishes Normal and runs the selected case. The overhead `main` selects the mode sequence, smoke or watchdog run. The other `main` dispatches the selected scenario or repeats the worker-fault suite.

### [.milestone-cpu-meter.inc](../.milestone-cpu-meter.inc)

The instrumented manager includes this Pi 4 CPU meter. `find_idle_clocks` discovers the four named idle threads. `clock_ns` reads an accounting clock. `read_core_load` converts idle-time deltas into per-core busy percentages after warm-up. `record_spike` captures the qualifying decision and measurement window. `export_spike` writes them to the trial CSV.

### [.milestone-handoff.inc](../.milestone-handoff.inc)

`thread_sample` reads a specified TID from procfs. `run_handoff` holds Normal for ten seconds and compares per-thread CPU time, scheduling, caps and critical-job evidence across the interval. `print_trace` reproduces the worker-effects and job logs in the handoff output.

### [.milestone-ham-suite.c](../.milestone-ham-suite.c)

`find_utility` locates HAM executables, `run` launches a utility and checks its status, `start_ham` starts the manager, and `show_ham_state` reports its state. `run_trial` writes and executes one embedded test. `main` enables heartbeat monitoring and runs the Throttled, Critical and Recovery cases, accumulating their exit statuses.

### [tests/overhead-shared.h](../tests/overhead-shared.h)

Defines the shared sample slot, process-shared mutex and atomic run counters. `open_overhead` creates or joins the file-backed mapping and initializes synchronization and counters for its creator.

### [tests/overhead-trial.inc](../tests/overhead-trial.inc)

Runs the CPU-overhead experiment at the same 25 ms acquisition interval in all three modes. A busy-waits, B wakes on timer pulses and C suppresses redundant notifications during steady operation.

- `nsclock` reads a clock, `timespec_ns` converts a nanosecond value to a timespec, `wait_until` sleeps to an absolute time, and `start_sampler_timer` configures periodic pulse delivery.
- `monitor_loop` replays the input into the shared slot and notifies the supervisor according to the selected mode. `watchdog_loop` checks supervisor progress and sends a forced-off pulse after a trip.
- `start_aux` starts the monitor and watchdog processes. `stop_aux` stops and reaps them. `prepare_overhead` prepares the shared file and trial assets.
- `counts_now` snapshots acquisition and watchdog counters. `idle_ids` obtains per-core idle clocks. `measure_overhead` scores the observation window using role CPU times, notice counts, critical deadlines and the final caps. `measure_watchdog_fault` runs the supervisor-stop check and audits jobs for that interval.

## Regression checks

### [tests/replay.c](../tests/replay.c)

`baseline` starts the local policy and feeds healthy data until Normal. `step` advances synthetic time by one tick and optionally refreshes the thermal acquisition. `acknowledge` applies the current decision and `run` repeats healthy steps. `main` exercises the thresholds, loss of freshness, invalid samples, lease expiry, recovery and protocol conversions, and invokes the metric checks.

### [tests/metric_checks.h](../tests/metric_checks.h)

Declares the shared `metric_checks` entry point for the replay driver and its metric assertions.

### [tests/metric_checks.c](../tests/metric_checks.c)

`metric_checks` feeds synthetic transitions, effects and jobs into the scorekeeper. It checks response latency, deadline misses, overshoot windows, transition spread, restoration and the explicit power-index path.

### [tests/manager_checks.c](../tests/manager_checks.c)

`main` verifies progress-sequence and timestamp rejection, expiry at the 50 ms boundary, independent worker acknowledgements and preservation of the first supervisor receipt.

### [tests/merge_checks.c](../tests/merge_checks.c)

`main` verifies watchdog expiry at the exact boundary, immutability of the forced-off timestamp, rejection of restored caps after a trip and the overhead experiment's result codes.

### [tests/threadctl_checks.c](../tests/threadctl_checks.c)

`inherit_probe` captures a child thread's inherited masks. `hold_probe` increments a counter so hold/continue can be observed. `exit_probe` provides a thread whose exit triggers a QNX pulse when an exit event is registered. `main` tests affinity and mask readback, inheritance across threads and spawned processes, invalid masks and the QNX thread-control operations.

## Build and operator scripts

The root [Makefile](../Makefile) builds the service, split runtime, enforcer, manager, monitors, console and checks. The [bench Makefile](../testbench/Makefile) builds the threaded workload and HAM helper. From an initialized QNX SDP environment run the following commands.

```sh
make BUILD_PROFILE=release
make -C testbench
```

### [.milestone-build.py](../.milestone-build.py)

Builds the selected target runners. `qcc` invokes the AArch64 QNX compiler and stops on a failed command. `write_assets` embeds built executables as byte arrays for staging on the target. The compiler enables `PG_BENCH_TRIAL` for the instrumented manager and workload, and `OVERHEAD_TRIAL` for the separate-monitor comparison.

### [scripts/jury-demo.sh](../scripts/jury-demo.sh)

`start_run` creates an isolated instance and starts its services and recorders. `wait_normal` waits for startup recovery. `show_status` formats the runtime snapshot using the awk helpers `value`, `number` and `reasons` to read fields, decode hexadecimal flags and label causes. `restore` requests normal inputs, `scores` captures the scorecard, and `cleanup` shuts down the demo's children.

### [scripts/validate-threadctl.sh](../scripts/validate-threadctl.sh)

Runs the target checks, scenarios and stall probes and collects jobs, samples, confirmations and scores for its own instance. `wait_state` polls the requested state with a bounded attempt count. `cleanup` stops the processes launched by the script.

### [scripts/validate-startup-latch.sh](../scripts/validate-startup-latch.sh)

Runs the threaded bench and watchdog without a supervisor, then checks that a positive manual grant is rejected and both caps stay closed. `cleanup` stops this test's children.

### [tests/analyze_threadctl.py](../tests/analyze_threadctl.py)

`analyze` audits the exported critical-job IDs, periodic releases, start/finish ordering and deadlines. `distribution` produces percentile and maximum values for response, release lateness and execution wall time.

### [tests/analyze_manager_stalls.py](../tests/analyze_manager_stalls.py)

`analyze` checks the recorded manager-stop trials against progress expiry and supervisor receipt times, rejects duplicate decisions and verifies the minimum trial count.

### [tests/test_manager_stall_analysis.py](../tests/test_manager_stall_analysis.py)

`ConfirmationAudit.row` creates an observation and `audit` runs the analyzer on a temporary log. `test_receipt_while_stopped` covers the accepted case. `test_moving_origin_cannot_hide_delay`, `test_late_receipt_fails` and `test_replayed_decision_is_not_another_trial` cover the three rejection cases.

### Windows remote console

Both [original launcher](../scripts/PowerGuard_Remote_CLI_Windows/powerguard_remote_cli/PowerGuard-Remote.ps1) and [updated launcher](../scripts/PowerGuard_Remote_CLI_Windows_FIXED/powerguard_remote_cli/PowerGuard-Remote.ps1) check for OpenSSH, copy the menu to the board and open an interactive SSH session. Each PowerShell script executes at top level.

The [original menu](../scripts/PowerGuard_Remote_CLI_Windows/powerguard_remote_cli/pg-remote-menu.sh) and [updated menu](../scripts/PowerGuard_Remote_CLI_Windows_FIXED/powerguard_remote_cli/pg-remote-menu.sh) share these functions. `have` checks an executable under `/tmp` and `line` prints the menu divider. `status` inspects processes and the live scorecard. `run_console` sends simulated input, `custom` prompts for its values, and `start_service` checks for an existing supervisor before starting one. `bench` queries the threaded workload, `bench_scenario` selects its scenario and `leds` starts the board-specific observer.

## Coding and verification scope

The submission uses braced control flow, explicit storage types in IPC records, named interface parameters and the repository's [formatter profile](../.clang-format). Both sides of the worker entry point include the same declaration. QNX APIs and the existing GNU C11 build settings remain part of the target contract. A formal MISRA compliance assessment and new board timing measurements are outside the verification recorded here.

The QNX AArch64 debug and release builds, standalone bench and instrumented runner builds were cross-compiled. The host policy replay, manager-progress checks, watchdog-latch checks and console example produced the same output as the submission baseline `9d9a994`. The four manager-stall analyzer tests passed. A target-compiled comparison also matched 537 enum values, type sizes, alignments, member widths and offsets across the policy, IPC and worker interfaces. See the [submission license](../LICENSE) for the team contribution terms and the original MIT notice.
