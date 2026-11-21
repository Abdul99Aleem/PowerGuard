#!/bin/sh
# Run beside the release bundle on QNX. Uses only its own isolated processes.
set -eu
if [ "$#" -ne 5 ]; then
    echo 'Usage: sh validate-real-thermal.sh C|mC IDLE_TID_CPU0 IDLE_TID_CPU1 IDLE_TID_CPU2 IDLE_TID_CPU3' >&2
    exit 2
fi
units=$1
shift
case "$units" in C|mC) ;; *) exit 2 ;; esac
cd "$(dirname "$0")"
bundle=$(pwd)
for tool in PowerGuard pg-runtime pg-manager pg-enforcer pg-thermal pg-console pg-validate pgbench; do
    test -x "./$tool" || { echo "Missing executable: $tool" >&2; exit 1; }
done
export PG_INSTANCE="real_thermal_$$"
export PG_REAL_THERMAL=1
out="$bundle/real-thermal-evidence-$$"
mkdir "$out"
runtime= watchdog= supervisor= manager= enforcer= monitor= recorder= sampler=
cleanup() {
    if [ -n "$monitor" ]; then
        kill -CONT "$monitor" 2>/dev/null || :
        kill -TERM "$monitor" 2>/dev/null || :
        wait "$monitor" 2>/dev/null || :
        monitor=
    fi
    for child in "$manager" "$enforcer" "$watchdog" "$recorder" "$sampler" "$supervisor"; do
        if [ -n "$child" ]; then kill -TERM "$child" 2>/dev/null || :; fi
    done
    if [ -n "$runtime" ]; then ./pg-runtime stop >/dev/null 2>&1 || kill -TERM "$runtime" 2>/dev/null || :; fi
    for child in "$monitor" "$manager" "$enforcer" "$watchdog" "$recorder" "$sampler" "$supervisor" "$runtime"; do
        if [ -n "$child" ]; then wait "$child" 2>/dev/null || :; fi
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM
wait_for() {
    target=$1
    attempts=0
    while ! ./pg-validate "$target" >"$out/last-check.txt" 2>&1; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 15 ]; then
            cat "$out/last-check.txt"
            ./pg-runtime status || :
            echo "FAIL: expected $target; inspect $out" >&2
            return 1
        fi
        sleep 1
    done
    cat "$out/last-check.txt" | tee -a "$out/assertions.txt"
}
cat /dev/thermal >"$out/device-probe.txt"
./pg-runtime serve "$@" >"$out/runtime.txt" 2>&1 & runtime=$!
sleep 1
./pg-runtime watchdog >"$out/watchdog.txt" 2>&1 & watchdog=$!
./pg-runtime record >"$out/jobs.csv" 2>"$out/jobs-errors.txt" & recorder=$!
./pg-runtime samples >"$out/runtime-samples.csv" 2>"$out/samples-errors.txt" & sampler=$!
./PowerGuard 25 >"$out/supervisor.txt" 2>&1 & supervisor=$!
sleep 1
./pg-enforcer >"$out/enforcer.txt" 2>&1 & enforcer=$!
./pg-manager >"$out/manager.txt" 2>&1 & manager=$!
mkdir "$out/monitor"
(cd "$out/monitor"; exec "$bundle/pg-thermal" pulse "$units" 120 75 "$@" runtime) >"$out/thermal.txt" 2>&1 & monitor=$!
echo "REAL /dev/thermal; 25 ms pulse acquisition; instance $PG_INSTANCE; evidence $out"
wait_for normal
./pg-runtime status | tee "$out/normal-status.txt"
grep 'temp=.*source=REAL' "$out/normal-status.txt" >/dev/null
if ./pgbench scenario ramp >"$out/rejected-simulation.txt" 2>&1; then
    echo 'FAIL: simulated ramp was accepted in real-temperature mode' >&2
    exit 1
fi
echo 'PASS simulated thermal ramp rejected in real-temperature mode' | tee -a "$out/assertions.txt"
./pgbench scenario spike >"$out/spike-command.txt"
sleep 1
if ! ./pg-validate throttled >"$out/spike-check.txt" 2>&1; then
    ./pg-validate critical >"$out/spike-check.txt" 2>&1
fi
cat "$out/spike-check.txt" | tee -a "$out/assertions.txt"
./pg-runtime status >"$out/spike-status.txt"
./pgbench scenario normal >"$out/normal-command.txt"
wait_for normal
echo 'Pausing only the real temperature producer; manager and job forwarding stay alive.'
kill -STOP "$monitor"
sleep 1
wait_for critical
./pg-runtime status | tee "$out/frozen-status.txt"
grep 'temp=.*source=REAL' "$out/frozen-status.txt" >/dev/null
kill -CONT "$monitor"
wait_for normal
./pg-runtime status | tee "$out/recovered-status.txt"
echo 'Capturing 5 seconds of physical temperature and critical timing after restoration.'
sleep 5
wait_for normal
./pg-runtime status >"$out/end-status.txt"
./pg-console --live scores 1 Normal Warning | tee "$out/scores.txt"
grep 'Restriction within .*PASS' "$out/scores.txt" >/dev/null
kill -0 "$manager"
kill -0 "$enforcer"
kill -0 "$monitor"
kill -TERM "$monitor"
wait "$monitor"
monitor=
test -s "$out/monitor/samples.csv"
grep '^read_errors,0$' "$out/monitor/summary.csv" >/dev/null
grep '^overflow,0$' "$out/monitor/summary.csv" >/dev/null
printf 'PASS real thermal source, measured CPU restriction, producer expiry, staged recovery and critical-job checks\n' | tee "$out/result.txt"
