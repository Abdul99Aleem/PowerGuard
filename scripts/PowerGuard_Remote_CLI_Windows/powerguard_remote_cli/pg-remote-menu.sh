#!/bin/sh
# PowerGuard QNX remote menu; run from a real interactive SSH session.
# This driver NEVER sets a desired supervisor state or claims a scored target run.
PATH=/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/tmp:$PATH
cd /tmp || exit 1

have() { [ -f "/tmp/$1" ] && [ -x "/tmp/$1" ]; }
line() { printf '\n%s\n' '------------------------------------------------------------'; }
status() {
    line
    echo 'Target identity / running processes:'
    uname -a 2>/dev/null || :
    pidin ar 2>/dev/null | grep -E 'PowerGuard|pg-console|pgbench|pg-state-leds|watchdog|manager|critical|nc[12]' || echo '(no matching processes visible)'
    echo 'Binaries in /tmp:'
    ls -l /tmp/PowerGuard /tmp/pg-console /tmp/pg-check /tmp/pgbench /tmp/pg-state-leds /tmp/pg-live-record 2>/dev/null || :
    echo 'IPC probe (requires live supervisor):'
    if have pg-console; then /tmp/pg-console --live scores; else echo 'pg-console missing'; fi
    echo 'NOTE: a scorecard query does not establish worker/manager/critical integration.'
}
run_console() {
    if ! have pg-console; then echo 'MISSING /tmp/pg-console. Deploy this executable from Momentics.'; return 1; fi
    echo "INPUT (SIMULATED): $*"
    /tmp/pg-console --live "$@"
}
custom() {
    printf 'Temperature C (-40..125): '; read temp || return
    printf 'CPU (0..100 or 4 comma-separated cores): '; read cpu || return
    printf 'Power index (0..100): '; read power || return
    printf 'Quality (good/stale/frozen/missing): '; read quality || return
    printf 'Duration ms (multiple of 10, 10..600000): '; read duration || return
    case "$quality" in good|stale|frozen|missing) : ;; *) echo 'Invalid quality'; return;; esac
    case "$duration" in ''|*[!0-9]*) echo 'Invalid duration'; return;; esac
    run_console "$temp" "$cpu" "$power" "$quality" "$duration"
}
start_service() {
    if ! have PowerGuard; then echo 'MISSING /tmp/PowerGuard'; return; fi
    if pidin ar 2>/dev/null | grep -q '[P]owerGuard'; then echo 'PowerGuard appears to be running. Do not launch a duplicate.'; return; fi
    echo 'Starting supervisor as this SSH user with FIFO priority 20.'
    ( cd /tmp; /tmp/PowerGuard 20 >/tmp/powerguard-supervisor.log 2>&1 </dev/null & )
    echo 'Check menu option 1 and /tmp/powerguard-supervisor.log for startup or permission failures.'
}
bench() {
    if ! have pgbench; then echo 'MISSING /tmp/pgbench'; return; fi
    echo 'Standalone workload-generator status; NOT proof it is linked to the supervisor:'
    /tmp/pgbench status
}
bench_scenario() {
    if ! have pgbench; then echo 'MISSING /tmp/pgbench'; return; fi
    echo "Standalone workload scenario '$1' (actual live IPC coupling must be verified separately)."
    /tmp/pgbench scenario "$1"
}
leds() {
    if ! have pg-state-leds; then echo 'MISSING /tmp/pg-state-leds'; return; fi
    printf 'Board [pi4/pi5]: '; read board || return
    case "$board" in pi4|pi5) /tmp/pg-state-leds --board "$board" ;; *) echo 'Must be pi4 or pi5';; esac
}
while :; do
    line
    echo 'POWERGUARD - QNX live CLI (root SSH session)'
    echo ' 1  Inspect binaries, processes, live IPC and scorecard'
    echo ' 2  Start supervisor only if not already running'
    echo ' 3  Normal-input preview       (55C, CPU30, PWR30, 3500ms)'
    echo ' 4  Warning-input preview      (65C, CPU30, PWR30, 600ms)'
    echo ' 5  Throttled-input preview    (70C, CPU30, PWR30, 600ms)'
    echo ' 6  Critical-input preview     (78C, CPU30, PWR30, 600ms)'
    echo ' 7  Injected high CPU reading  (55C, CPU86, PWR30, 600ms)'
    echo ' 8  Injected power index       (55C, CPU30, PWR80, 600ms)'
    echo ' 9  Stale acquisition replay   (55C, CPU30, PWR30, 600ms)'
    echo '10  Explicit frozen quality    (55C, CPU30, PWR30, 600ms)'
    echo '11  Healthy recovery input     (55C, CPU30, PWR30, 4000ms)'
    echo '12  Custom source input via pg-console --live'
    echo '13  Query live supervisor scorecard'
    echo '14  Supervisor log tail'
    echo '15  Standalone pgbench worker status'
    echo '16  Standalone pgbench scenario: spike'
    echo '17  Standalone pgbench scenario: ramp'
    echo '18  Standalone pgbench scenario: freeze'
    echo '19  Run 5-state LED observer (foreground; Ctrl+C returns)'
    echo '20  Open native interactive pg-console --live'
    echo ' 0  Exit SSH menu'
    printf 'Choice: '
    IFS= read choice || exit 0
    case "$choice" in
      1) status ;;
      2) start_service ;;
      3) run_console 55 30 30 good 3500 ;;
      4) run_console 65 30 30 good 600 ;;
      5) run_console 70 30 30 good 600 ;;
      6) run_console 78 30 30 good 600 ;;
      7) run_console 55 86 30 good 600 ;;
      8) run_console 55 30 80 good 600 ;;
      9) run_console 55 30 30 stale 600 ;;
     10) run_console 55 30 30 frozen 600 ;;
     11) run_console 55 30 30 good 4000 ;;
     12) custom ;;
     13) if have pg-console; then /tmp/pg-console --live scores; else echo 'pg-console missing'; fi ;;
     14) tail -n 60 /tmp/powerguard-supervisor.log 2>/dev/null || echo 'No launch log found' ;;
     15) bench ;;
     16) bench_scenario spike ;;
     17) bench_scenario ramp ;;
     18) bench_scenario freeze ;;
     19) leds ;;
     20) if have pg-console; then /tmp/pg-console --live; else echo 'pg-console missing'; fi ;;
      0) exit 0 ;;
      *) echo 'Unknown option' ;;
    esac
 done
