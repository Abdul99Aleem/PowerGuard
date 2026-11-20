#ifndef PG_BENCH_PROTOCOL_H
#define PG_BENCH_PROTOCOL_H

#include <stdint.h>
#include "pg_threadctl.h"
#include "pg_names.h"

/*
 * Minimal local QNX name_attach/name_open + MsgSend contract.
 * A supervisor can send PG_CMD_LEVEL and PG_CMD_STATE messages.
 * All timestamps are CLOCK_MONOTONIC, all timing figures are measured.
 */
#define PG_BENCH_NAME pg_bench_name()
#define PG_BENCH_TYPE 0x2001u
#define PG_BENCH_VERSION 4u

enum pg_command {
    PG_CMD_STATUS = 1,
    PG_CMD_SCENARIO = 2,
    PG_CMD_LEVEL = 3,
    PG_CMD_STATE = 4,
    PG_CMD_STOP = 5,
    /* Exact NC1/NC2 caps.  This is used by pg-manager; PG_CMD_LEVEL remains
       available for the original manual demo CLI. */
    PG_CMD_CAPS = 6,
    PG_CMD_LEASED_CAPS,
    PG_CMD_JOBS,
    PG_CMD_WATCHDOG,
    PG_CMD_ENFORCER
};

enum pg_scenario {
    PG_SC_NORMAL = 0,
    PG_SC_RAMP = 1,
    PG_SC_SPIKE = 2,
    PG_SC_FREEZE = 3
};

/* These are REPORTS from the supervisor, not states decided by this testbench. */
enum pg_bench_state {
    PG_ST_UNKNOWN = 0,
    PG_ST_NORMAL = 1,
    PG_ST_WARNING = 2,
    PG_ST_THROTTLED = 3,
    PG_ST_CRITICAL = 4,
    PG_ST_RECOVERY = 5
};

/* Level 0 = both noncritical workers on; 1 = NC2 off; 2 = NC1 and NC2 off. */
typedef struct {
    uint16_t type;
    uint16_t version;
    uint32_t command;
    int32_t arg;
    int32_t extra;
} pg_request_t;

typedef struct {
    pg_request_t request;
    uint64_t epoch, decision_id, until_ns;
} pg_cap_request_t;

typedef struct {
    int32_t error;                /* 0 = success, otherwise POSIX errno */
    int32_t scenario;
    int32_t level_requested;
    int32_t level_applied;
    int32_t nc1_cap_pct;
    int32_t nc2_cap_pct;
    int32_t supervisor_state;
    int32_t recovery_percent;
    int32_t critical_fifo_ok;
    int32_t process_cpu_per_core_tenths; /* 1000 = 100.0% of one CPU core */
    int32_t temperature_milli_c; /* SIMULATED, not a board reading */
    uint32_t sensor_age_ms;
    uint64_t monotonic_ns;
    uint64_t critical_jobs;
    uint64_t critical_misses;
    uint64_t critical_max_response_us;
    uint64_t critical_max_jitter_us;
    uint64_t last_actuation_us;   /* command receipt -> BOTH worker acknowledgments */
    uint64_t last_effective_ns;   /* CLOCK_MONOTONIC timestamp of BOTH-worker ACK */
    uint64_t detection_to_actuation_us; /* 0 when no qualifying detection */
    uint64_t recovery_to_full_ms; /* 0 when not measured */
    uint64_t last_job_id;
    uint64_t last_job_release_ns;
    uint64_t last_job_deadline_ns;
    uint64_t last_job_finish_ns;
    uint64_t thermal_acquired_ns, cpu_window_end_ns;
    uint64_t applied_epoch, applied_decision_id;
    uint32_t features, ready_mask, watchdog_ok, cpu_valid;
    int32_t core_busy_pct[4];
    pg_thread_report placement[5]; /* coordinator, critical, NC1, NC2, enforcer */
    uint64_t worker_seen_ns[3], worker_effect_ns[2];
    uint64_t first_job_id, job_overwrites, worker_dropped_reports;
    uint64_t enforcer_seen_ns;
    uint32_t enforcer_ok, forced_off; /* former reserved word; v4 layout unchanged */
} pg_status_t;

#define PG_FEATURE_THREADCTL 1u
#define PG_FEATURE_ENFORCER 2u
typedef struct { pg_request_t request; pg_thread_report placement; } pg_enforcer_request;
#define PG_JOB_BATCH 16u
typedef struct { uint64_t id, release_ns, deadline_ns, finish_ns, start_ns; } pg_job_record;
typedef struct {
    pg_request_t request;
    uint64_t after_id;
} pg_jobs_request;
typedef struct {
    int32_t error;
    uint32_t count;
    uint64_t overwritten;
    pg_job_record jobs[PG_JOB_BATCH];
} pg_jobs_reply;

#endif
