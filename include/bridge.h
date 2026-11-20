#ifndef POWERGUARD_BRIDGE_H
#define POWERGUARD_BRIDGE_H

#include "policy.h"
#include "pgbench_protocol.h"

/* pgbench reserves zero for UNKNOWN; the supervisor starts at NORMAL. */
static inline int pg_bench_state_from_policy(enum pg_state state) {
    return (int)state + PG_ST_NORMAL;
}

#endif
