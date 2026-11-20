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
    uint16_t type, version;
    uint32_t role; /* 0 critical; 1 NC1; 2 NC2 */
    pg_thread_report placement;
    uint64_t epoch, decision_id, effective_ns, seen_ns;
    int32_t applied_cap, fault;
    uint64_t dropped;
    pg_job_record job;
} pg_worker_message;
typedef struct {
    int32_t error, cap, demand_pct, stop;
    uint64_t epoch, decision_id, until_ns;
} pg_worker_reply;
#endif
