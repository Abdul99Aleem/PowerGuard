/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * runtime/protocol.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef PG_RUNTIME_PROTOCOL_H
#define PG_RUNTIME_PROTOCOL_H
#include "pgbench_protocol.h"
#define PG_RUNTIME_TYPE 0x2401u
#define PG_RUNTIME_VERSION 1u
#define PG_WORKER_PERIOD UINT64_C(5000000)
#define PG_CRITICAL_PERIOD UINT64_C(10000000)
#define PG_CRITICAL_DEADLINE UINT64_C(5000000)
#define PG_HEARTBEAT_LIMIT UINT64_C(50000000)
#define PG_WATCHDOG_LIMIT UINT64_C(100000000)

typedef struct {
    uint16_t type;
    uint16_t version;
    uint32_t role;
    pg_thread_report placement;
    uint64_t epoch;
    uint64_t decision_id;
    uint64_t effective_ns;
    uint64_t seen_ns;
    int32_t applied_cap;
    int32_t fault;
    uint64_t dropped;
    pg_job_record job;
} pg_worker_message;

typedef struct {
    int32_t error;
    int32_t cap;
    int32_t demand_pct;
    int32_t stop;
    uint64_t epoch;
    uint64_t decision_id;
    uint64_t until_ns;
} pg_worker_reply;

int pg_worker_main(unsigned role, unsigned iterations);
#endif
