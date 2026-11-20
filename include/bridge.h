#ifndef POWERGUARD_BRIDGE_H
#define POWERGUARD_BRIDGE_H

#include "policy.h"
#include "pgbench_protocol.h"

/* pgbench publishes an absolute deadline; PG_JOB v1 takes a duration. */
static inline uint64_t pg_job_budget_ns(uint64_t release, uint64_t deadline_at) {
    return deadline_at > release ? deadline_at - release : 0;
}

/* pgbench reserves zero for UNKNOWN; the policy enum assigns zero to NORMAL. */
static inline int pg_bench_state_from_policy(enum pg_state state) {
    return (int)state + PG_ST_NORMAL;
}

#endif
