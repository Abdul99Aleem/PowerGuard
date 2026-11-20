#!/bin/sh
# B5 on the board: start a fresh run, stall the supervisor for 1 s with
# SIGSTOP, then read the watchdog's timing and the state after it resumes.
#
# Copy this next to the built binaries (PowerGuard, pg-manager, pgbench,
# pg-watchdog, pg-probe) and run it as root:  sudo ./b5-repeat.sh [RUNS]
# It signals only the PIDs it started, so it is safe on a shared board,
# but check `pidin arg` first: a second PowerGuard takes the same names.
cd "$(dirname "$0")" || exit 1
runs=${1:-5}
i=1
while [ "$i" -le "$runs" ]; do
    ./pgbench serve /tmp/powerguard/bench.csv > bench.log 2>&1 & bench=$!
    sleep 1
    ./PowerGuard 42 > pg.log 2>&1 & sup=$!
    sleep 1
    ./pg-manager > mgr.log 2>&1 & mgr=$!
    sleep 1
    ./pg-watchdog 44 > wd.log 2>&1 & wd=$!
    sleep 5
    before=$(./pgbench status | grep -o "supervisor=[A-Z]*")
    snap=$(./pg-probe 1 | cut -c5-70)
    kill -STOP "$sup"
    sleep 1
    kill -CONT "$sup"
    sleep 3
    after=$(./pgbench status | grep -oE "supervisor=[A-Z]*|NC[12] cap: [0-9]+%|misses=[0-9]+" | tr '\n' ' ')
    echo "run $i: $before [$snap] | $(grep 'both off' wd.log) | after resume: $after"
    ./pgbench stop > /dev/null 2>&1
    kill -TERM "$wd" "$mgr" "$sup" 2> /dev/null
    sleep 1
    kill -KILL "$bench" 2> /dev/null
    i=$((i + 1))
done
