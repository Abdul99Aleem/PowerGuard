#!/bin/sh
# Run beside the release bundle on QNX. Uses only its own isolated processes.
set -eu
if [ "$#" -ne 5 ]; then
    echo 'Usage: sh low-temperature-demo.sh C|mC IDLE_TID_CPU0 IDLE_TID_CPU1 IDLE_TID_CPU2 IDLE_TID_CPU3' >&2
    exit 2
fi
units=$1
shift
case "$units" in C|mC) ;; *) exit 2 ;; esac
cd "$(dirname "$0")"
bundle=$(pwd)
for tool in PowerGuard-thermal-demo pg-demo-heater pg-thermal-demo-check pg-runtime pg-manager pg-enforcer pg-thermal pg-console pg-validate; do
    test -x "./$tool" || { echo "Missing executable: $tool" >&2; exit 1; }
done
export PG_INSTANCE="low_thermal_$$"
export PG_REAL_THERMAL=1
out="$bundle/low-thermal-evidence-$$"
mkdir "$out"
runtime= watchdog= supervisor= manager= enforcer= monitor= recorder= sampler= heater=
cleanup() {
    if [ -n "$heater" ]; then kill -TERM "$heater" 2>/dev/null || :; wait "$heater" 2>/dev/null || :; fi
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
./PowerGuard-thermal-demo 25 >"$out/supervisor.txt" 2>&1 & supervisor=$!
sleep 1
./pg-enforcer >"$out/enforcer.txt" 2>&1 & enforcer=$!
./pg-manager >"$out/manager.txt" 2>&1 & manager=$!
mkdir "$out/monitor"
(cd "$out/monitor"; exec "$bundle/pg-thermal" pulse "$units" 120 75 "$@" runtime) >"$out/thermal.txt" 2>&1 & monitor=$!
echo "LOW-TEMPERATURE DEMO ONLY: clear <=56.0 C; Warning >=56.5; Throttled >=57.0; Critical >=57.5"
echo "REAL /dev/thermal; 25 ms pulses; instance $PG_INSTANCE; evidence $out"
./pg-thermal-demo-check >"$out/profile-check.txt"
wait_for normal
./pg-runtime status >"$out/normal-status.txt"
./pg-demo-heater >"$out/heater.txt" 2>&1 & heater=$!
echo 'Applying a bounded external heating disturbance: 30% duty on each core, FIFO1, 20 seconds.'
seen_critical=0
step=0
while [ "$step" -lt 60 ]; do
    sleep 1
    ./pg-runtime status >"$out/current-status.txt"
    cat "$out/current-status.txt" >>"$out/observations.txt"
    awk '/^temp=|^CPU=|^requested_caps=/ { print }' "$out/current-status.txt"
    if grep 'state=3 causes=0x1 ' "$out/current-status.txt" >/dev/null; then seen_critical=1; fi
    step=$((step + 1))
    if [ "$step" -ge 21 ] && [ "$seen_critical" -eq 1 ] && ./pg-validate normal >"$out/recovery-check.txt" 2>&1; then break; fi
done
wait "$heater"
heater=
if [ "$seen_critical" -ne 1 ]; then echo 'FAIL: no temperature-only Critical observed'; exit 1; fi
wait_for normal
./pg-runtime status >"$out/recovered-status.txt"
./pg-console --live scores 1 Normal Warning | tee "$out/scores.txt"
grep 'Restriction within .*PASS' "$out/scores.txt" >/dev/null
kill -TERM "$monitor"
wait "$monitor"
monitor=
grep '^read_errors,0$' "$out/monitor/summary.csv" >/dev/null
grep '^overflow,0$' "$out/monitor/summary.csv" >/dev/null
echo 'PASS real temperature crossed the lowered Critical threshold, stopped both NC workloads, then recovered to Normal' | tee "$out/result.txt"
