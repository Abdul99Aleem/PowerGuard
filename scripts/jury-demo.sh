#!/bin/sh
# POWERGUARD  /  TEAM SCHEDULIX
# --------------------------------------------------------------------------
# jury-demo.sh
# Original software (c) 2026 Abdul Aleem. MIT License.
# Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.

set -u
if [ "$#" -ne 4 ]; then
    echo 'On the Pi: sh jury-demo.sh IDLE_TID_CPU0 IDLE_TID_CPU1 IDLE_TID_CPU2 IDLE_TID_CPU3' >&2
    exit 2
fi
cd "$(dirname "$0")" || exit 1
for tool in pg-runtime PowerGuard pg-manager pg-enforcer pgbench pg-console pg-validate pg-stall-check pg-supervisor-check; do
    if [ ! -x "./$tool" ]; then echo "Missing executable: $tool. Place this menu beside the release binaries." >&2; exit 1; fi
done
base="jury-evidence-$$"
mkdir "$base" || exit 1
runtime= watchdog= supervisor= manager= enforcer= recorder= sampler=
round=0 latched=0
cleanup() {
    for child in "$manager" "$watchdog" "$supervisor"; do
        if [ -n "$child" ]; then kill -CONT "$child" 2>/dev/null || :; fi
    done
    for child in "$recorder" "$sampler" "$manager" "$enforcer" "$watchdog" "$supervisor"; do
        if [ -n "$child" ]; then kill -TERM "$child" 2>/dev/null || :; fi
    done
    if [ -n "$runtime" ]; then ./pg-runtime stop >/dev/null 2>&1 || kill -TERM "$runtime" 2>/dev/null || :; fi
    for child in "$recorder" "$sampler" "$manager" "$enforcer" "$watchdog" "$supervisor" "$runtime"; do
        if [ -n "$child" ]; then wait "$child" 2>/dev/null || :; fi
    done
    runtime= watchdog= supervisor= manager= enforcer= recorder= sampler=
}
trap cleanup EXIT
trap 'exit 130' INT TERM
wait_normal() {
    attempts=0
    while ! ./pg-validate normal >"$out/readiness.txt" 2>&1; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 20 ]; then cat "$out/readiness.txt"; echo "Readiness failed. Logs: $out"; return 1; fi
        sleep 1
    done
}
show_status() {
    ./pg-runtime status | awk '
        BEGIN {name[0]="Normal";name[1]="Warning";name[2]="Throttled";name[3]="Critical";name[4]="Recovery"}
        function value(key, j,a) {for(j=1;j<=NF;j++){split($j,a,"=");if(a[1]==key)return a[2]}return "?"}
        function number(s, j,n) {sub(/^0x/,"",s);n=0;for(j=1;j<=length(s);j++)n=16*n+index("0123456789abcdef",substr(tolower(s),j,1))-1;return n}
        function reasons(n, r) {
            if(!n)return "none";r=""
            if(n%2)r=r "temperature; "
            if(int(n/2)%2)r=r "CPU load; "
            if(int(n/4)%2)r=r "demand index; "
            if(int(n/8)%2)r=r "stale/invalid temperature; "
            if(int(n/16)%2)r=r "CPU data unavailable; "
            if(int(n/32)%2)r=r "effect unconfirmed; "
            if(int(n/64)%2)r=r "manager progress lost; "
            if(int(n/128)%2)r=r "watchdog latched; "
            return r
        }
        /^READY / {next}
        /^ready=/ {printf "Health: workers %s | watchdog %s | enforcer %s | CPU meter %s\n",value("ready")=="0x7"?"READY":"NOT READY",value("watchdog")==1?"OK":"WAIT",value("enforcer")==1?"OK":"WAIT",value("cpu_valid")==1?"OK":"WAIT";next}
        /^CPU=/ {printf "CPU busy by core: %s %% | Applied caps: NC1 %s%%, NC2 %s%%\n",value("CPU"),value("NC1"),value("NC2");next}
        /^critical_jobs=/ {printf "Critical jobs: %s | Misses: %s | Maximum response: %s us (deadline 5000 us)\n",value("critical_jobs"),value("misses"),value("max_response_us");next}
        /^temp=/ {printf "State: %s | Temperature: %s [%s] | Sensor age: %s ms | Demand index: %s\n",name[value("state")],value("temp"),value("source"),value("age_ms"),value("power_index");printf "Reason: %s\n",reasons(number(value("causes")));next}
        /^requested_caps=/ {printf "Target caps: %s | Last critical response: %s ms | Recovery clear time: %s ms | Missing job records: %s\n",value("requested_caps"),value("last_critical_latency_ms"),value("recovery_clear_ms"),value("job_gaps");next}
        {print}'
}
start_run() {
    cleanup
    round=$((round + 1))
    export PG_INSTANCE="jury_$$_$round"
    out="$base/run-$round"
    mkdir "$out" || return 1
    latched=0
    echo "Starting PowerGuard. Source: SIMULATED temperature; REAL per-core CPU."
    ./pg-runtime serve "$@" >"$out/runtime.txt" 2>&1 & runtime=$!
    sleep 1
    kill -0 "$runtime" 2>/dev/null || { cat "$out/runtime.txt"; return 1; }
    ./pg-runtime watchdog >"$out/watchdog.txt" 2>&1 & watchdog=$!
    ./pg-runtime record >"$out/jobs.csv" 2>"$out/jobs-errors.txt" & recorder=$!
    ./pg-runtime samples >"$out/samples.csv" 2>"$out/samples-errors.txt" & sampler=$!
    ./PowerGuard 25 >"$out/supervisor.txt" 2>&1 & supervisor=$!
    sleep 1
    ./pg-enforcer >"$out/enforcer.txt" 2>&1 & enforcer=$!
    ./pg-manager >"$out/manager.txt" 2>&1 & manager=$!
    wait_normal || return 1
    echo "READY: Normal, both noncritical workloads available. Evidence: $out"
    show_status
}
restore() {
    if [ "$latched" -eq 1 ]; then echo 'Watchdog latch is active. Choose 9 for a new run.'; return 1; fi
    ./pgbench scenario normal >"$out/scenario.txt" || return 1
    echo 'Recovery: keep all inputs clear; NC1 returns first, then NC2.'
    wait_normal && show_status
}
scores() {
    if [ "$latched" -eq 1 ]; then
        echo 'Supervisor was deliberately paused: its late IPC receipt is retained.'
        echo "Use the separate 270 ms watchdog result and $out/scores-before-watchdog.txt."
    fi
    ./pg-console --live scores 1 Normal Warning | tee "$out/scores.txt"
    echo "Raw critical jobs, samples and confirmations: $out"
}
start_run "$@" || exit 1
while :; do
    printf '\nPOWERGUARD | Jury demo | %s\n' "$PG_INSTANCE"
    printf '%s\n' '1  Show system status' '2  CPU load spike' '3  Temperature ramp (about 32 seconds)' \
        '4  Freeze temperature input (5 seconds)' '5  Restore and watch recovery' \
        '6  Prove manager-stall confirmation within 20 ms' '7  Show scorecard and evidence' \
        '8  Prove watchdog latch (last fault; new run needed)' '9  Start a fresh demo run' '0  Exit and stop demo'
    printf 'Choose: '
    if ! IFS= read -r choice; then break; fi
    case "$choice" in
        1) show_status ;;
        2) if restore; then
               ./pg-console --live trial 1 Warning >"$out/trial.txt" 2>&1 || :
               ./pgbench scenario spike >"$out/scenario.txt" && sleep 1 && show_status
               echo 'NC2 yields first. Choose 5 to restore.'
           fi ;;
        3) if restore; then
               ./pgbench scenario ramp >"$out/scenario.txt" || continue
               tick=0
               while [ "$tick" -lt 6 ]; do sleep 5; show_status; tick=$((tick + 1)); done
               sleep 2; show_status
               echo 'Warning at 65 C, NC2 off at 70 C, both off at 75 C. Temperature is simulated.'
           fi ;;
        4) if restore; then
               ./pgbench scenario freeze >"$out/scenario.txt" && sleep 5 && show_status
               echo 'Sensor age increases; stale input closes the noncritical workloads.'
           fi ;;
        5) restore || : ;;
        6) if restore; then
               echo 'Manager paused for 1 second. Origin: 50 ms progress expiry; receipt limit: 20 ms.'
               if ./pg-stall-check "$manager" >"$out/manager-stall.txt"; then
                   cat "$out/manager-stall.txt"; wait_normal && show_status
               else cat "$out/manager-stall.txt"; echo 'FAIL: inspect the run logs.'; fi
           fi ;;
        7) scores ;;
        8) if restore; then
               ./pg-console --live scores 1 Normal Warning >"$out/scores-before-watchdog.txt"
               latched=1
               echo 'Supervisor paused. Both off/latch must be observed within 270 ms of last progress.'
               if ./pg-supervisor-check "$supervisor" >"$out/watchdog-latch.txt"; then
                   cat "$out/watchdog-latch.txt"
               else cat "$out/watchdog-latch.txt"; echo 'FAIL: inspect the run logs.'; fi
               show_status; echo 'Capacity stays off after resume. Choose 9 to start a new run.'
           fi ;;
        9) start_run "$@" || exit 1 ;;
        0) echo "Demo finished. Evidence saved in $base"; break ;;
        *) echo 'Please choose a number from 0 to 9.' ;;
    esac
done
