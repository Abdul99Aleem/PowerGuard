#!/bin/sh
# Run beside the newly staged binaries. Only processes started here are stopped.
# Explicit idle TIDs are validated against procfs before any worker is enabled.
set -eu
if [ "$#" -ne 4 ]; then
    echo 'usage: sh validate-threadctl.sh IDLE_TID_CPU0 IDLE_TID_CPU1 IDLE_TID_CPU2 IDLE_TID_CPU3' >&2
    exit 2
fi
cd "$(dirname "$0")"
export PG_INSTANCE="threadctl_test_$$"
out="threadctl-results-$$"
mkdir "$out"
runtime= watchdog= supervisor= manager= enforcer= recorder= sampler=
cleanup() {
    for child in "$manager" "$watchdog" "$supervisor"; do
        if [ -n "$child" ]; then kill -CONT "$child" 2>/dev/null || :; fi
    done
    for child in "$recorder" "$sampler" "$manager" "$enforcer" "$watchdog" "$supervisor"; do
        if [ -n "$child" ]; then kill -TERM "$child" 2>/dev/null || :; fi
    done
    if [ -n "$runtime" ]; then
        ./pg-runtime stop >/dev/null 2>&1 || kill -TERM "$runtime" 2>/dev/null || :
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
wait_state() {
    attempts=0
    while ! ./pg-validate "$1" >"$out/pending-state.txt"; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 20 ]; then
            cat "$out/pending-state.txt" >>"$out/assertions.txt"
            ./pg-runtime status >"$out/failure-status.txt" 2>&1 || :
            return 1
        fi
        sleep 1
    done
    cat "$out/pending-state.txt" >>"$out/assertions.txt"
}
uname -a >"$out/platform.txt"
pidin syspage=cluster >>"$out/platform.txt"
./pg-threadctl-check >"$out/threadctl-check.txt" 2>&1
./pg-check >"$out/policy-check.txt" 2>&1
./pg-manager-check >"$out/manager-check.txt" 2>&1
./pg-merge-check >"$out/merge-check.txt" 2>&1
./pg-runtime serve "$@" >"$out/runtime.txt" 2>&1 & runtime=$!
sleep 1
kill -0 "$runtime"
./pg-runtime watchdog >"$out/watchdog.txt" 2>&1 & watchdog=$!
./pg-runtime record >"$out/jobs.csv" 2>"$out/jobs-errors.txt" & recorder=$!
./pg-runtime samples >"$out/samples.csv" 2>"$out/samples-errors.txt" & sampler=$!
./PowerGuard 25 >"$out/supervisor.txt" 2>&1 & supervisor=$!
sleep 1
./pg-enforcer >"$out/enforcer.txt" 2>&1 & enforcer=$!
./pg-manager >"$out/manager.txt" 2>&1 & manager=$!
wait_state normal
./pg-runtime status >"$out/start-status.txt"
pidin ar >"$out/processes.txt"
trial=0
while [ "$trial" -lt "${PG_TRIALS:-2}" ]; do
    ./pg-console --live trial 1 Warning >>"$out/assertions.txt"
    ./pgbench scenario spike >>"$out/scenarios.txt"
    sleep 1
    ./pg-validate throttled >>"$out/assertions.txt"
    ./pgbench scenario normal >>"$out/scenarios.txt"
    wait_state normal
    trial=$((trial + 1))
done
if [ "${PG_FULL_SCENARIOS:-1}" -eq 1 ]; then
./pgbench scenario ramp >>"$out/scenarios.txt"
sleep 32
./pg-validate critical >>"$out/assertions.txt"
./pgbench scenario normal >>"$out/scenarios.txt"
wait_state normal
./pgbench scenario freeze >>"$out/scenarios.txt"
sleep 5
./pg-validate critical >>"$out/assertions.txt"
./pgbench scenario normal >>"$out/scenarios.txt"
wait_state normal
fi
if [ "${PG_FAULT_TESTS:-1}" -eq 1 ]; then
stall_trial=0
while [ "$stall_trial" -lt "${PG_STALL_TRIALS:-1}" ]; do
    ./pg-stall-check "$manager" >>"$out/stall-confirmations.txt"
    wait_state normal
    stall_trial=$((stall_trial + 1))
done
kill -STOP "$watchdog"
sleep 1
./pg-validate closed >>"$out/assertions.txt"
kill -CONT "$watchdog"
wait_state normal
fi
restart_trial=0
while [ "$restart_trial" -lt "${PG_RESTART_TRIALS:-2}" ]; do
    kill -TERM "$manager"
    wait "$manager" || : # the deliberate SIGTERM may interrupt an IPC transaction
    ./pg-manager >"$out/manager-restart-$restart_trial.txt" 2>&1 & manager=$!
    wait_state normal
    kill -0 "$manager"
    restart_trial=$((restart_trial + 1))
done
./pg-console --live scores 1 Normal Warning >"$out/scores.txt"
./pg-runtime status >"$out/end-status.txt"
kill -0 "$manager"
kill -0 "$enforcer"
kill -0 "$recorder"
kill -0 "$sampler"
if [ "${PG_SUPERVISOR_FAULT_TESTS:-1}" -eq 1 ]; then
    ./pg-supervisor-check "$supervisor" >"$out/supervisor-latch.txt"
    ./pg-runtime status >"$out/latched-status.txt"
    ./pg-console --live scores 1 Normal Warning >"$out/latched-scores.txt"
    kill -0 "$enforcer"
    kill -0 "$manager"
fi
echo "Target assertions passed. Evidence: $out. Inspect scores for metric limits; this is not full qualification."
