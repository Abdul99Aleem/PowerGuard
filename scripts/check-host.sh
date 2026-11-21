#!/bin/sh
# Portable policy and evidence-accounting regressions; no QNX SDK required.
set -eu
cd "$(dirname "$0")/.."
compiler=${CC:-cc}
python=${PYTHON:-python3}
out=build/host-checks
mkdir -p "$out"

"$compiler" -std=c11 -Wall -Wextra -Werror -Iinclude -Itestbench \
    supervisor/policy.c supervisor/metrics.c tests/replay.c tests/metric_checks.c \
    -o "$out/pg-check"
"$out/pg-check"
"$compiler" -std=c11 -Wall -Wextra -Werror -Iinclude \
    supervisor/policy.c supervisor/metrics.c tests/manager_checks.c \
    -o "$out/pg-manager-check"
"$out/pg-manager-check"
"$compiler" -std=c11 -Wall -Wextra -Werror -Iinclude \
    supervisor/policy.c supervisor/metrics.c tests/merge_checks.c \
    -o "$out/pg-merge-check"
"$out/pg-merge-check"
"$python" -m unittest discover -s tests -p 'test_*.py' -v
