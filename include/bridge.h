/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * include/bridge.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef POWERGUARD_BRIDGE_H
#define POWERGUARD_BRIDGE_H

#include "policy.h"
#include "pgbench_protocol.h"

static inline uint64_t pg_job_budget_ns(uint64_t release, uint64_t deadline_at)
{
    return deadline_at > release ? deadline_at - release : 0;
}

static inline int pg_bench_state_from_policy(enum pg_state state)
{
    return (int)state + PG_ST_NORMAL;
}

#endif
