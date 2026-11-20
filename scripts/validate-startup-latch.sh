#!/bin/sh
# POWERGUARD  /  TEAM SCHEDULIX
# --------------------------------------------------------------------------
# validate-startup-latch.sh
# Original software (c) 2026 Abdul Aleem. MIT License.
# Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.

set -eu
cd "$(dirname "$0")"
export PG_INSTANCE="startup_latch_$$"
out="startup-latch-results-$$"
mkdir "$out"
bench= watchdog=
cleanup() {
    if [ -n "$watchdog" ]; then kill -TERM "$watchdog" 2>/dev/null || :; fi
    if [ -n "$bench" ]; then
        ./pgbench stop >/dev/null 2>&1 || kill -TERM "$bench" 2>/dev/null || :
        wait "$bench" || :
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
./pgbench serve "$out/bench.csv" >"$out/bench.txt" 2>&1 & bench=$!
sleep 1
./pg-runtime watchdog >"$out/watchdog.txt" 2>&1 & watchdog=$!
sleep 6
if ./pgbench level 0 >"$out/positive-command.txt" 2>&1; then
    echo 'FAIL startup latch admitted a manual positive workload command' >&2
    exit 1
fi
./pgbench status >"$out/status.txt"
grep -q 'NC1 cap: 0%.*NC2 cap: 0%' "$out/status.txt"
kill -0 "$bench"
kill -0 "$watchdog"
echo "PASS startup latch rejects positive admission without a policy epoch. Evidence: $out"
