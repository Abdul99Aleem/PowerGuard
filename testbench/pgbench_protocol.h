/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * testbench/pgbench_protocol.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef PG_BENCH_PROTOCOL_H
#define PG_BENCH_PROTOCOL_H

#include <stdint.h>
#include "pg_threadctl.h"
#include "pg_names.h"

#define PG_BENCH_NAME pg_bench_name()
#define PG_BENCH_TYPE 0x2001u
#define PG_BENCH_VERSION 4u

enum pg_command {
    PG_CMD_STATUS = 1,
    PG_CMD_SCENARIO = 2,
    PG_CMD_LEVEL = 3,
    PG_CMD_STATE = 4,
    PG_CMD_STOP = 5,

    PG_CMD_CAPS = 6,
    PG_CMD_LEASED_CAPS,
    PG_CMD_JOBS,
    PG_CMD_WATCHDOG,
    PG_CMD_ENFORCER
};

enum pg_scenario { PG_SC_NORMAL = 0, PG_SC_RAMP = 1, PG_SC_SPIKE = 2, PG_SC_FREEZE = 3 };

enum pg_bench_state {
    PG_ST_UNKNOWN = 0,
    PG_ST_NORMAL = 1,
    PG_ST_WARNING = 2,
    PG_ST_THROTTLED = 3,
    PG_ST_CRITICAL = 4,
    PG_ST_RECOVERY = 5
};

typedef struct {
    uint16_t type;
    uint16_t version;
    uint32_t command;
    int32_t arg;
    int32_t extra;
} pg_request_t;

typedef struct {
    pg_request_t request;
    uint64_t epoch;
    uint64_t decision_id;
    uint64_t until_ns;
} pg_cap_request_t;

typedef struct {
    int32_t error;
    int32_t scenario;
    int32_t level_requested;
    int32_t level_applied;
    int32_t nc1_cap_pct;
    int32_t nc2_cap_pct;
    int32_t supervisor_state;
    int32_t recovery_percent;
    int32_t critical_fifo_ok;
    int32_t process_cpu_per_core_tenths;
    int32_t temperature_milli_c;
    uint32_t sensor_age_ms;
    uint64_t monotonic_ns;
    uint64_t critical_jobs;
    uint64_t critical_misses;
    uint64_t critical_max_response_us;
    uint64_t critical_max_jitter_us;
    uint64_t last_actuation_us;
    uint64_t last_effective_ns;
    uint64_t detection_to_actuation_us;
    uint64_t recovery_to_full_ms;
    uint64_t last_job_id;
    uint64_t last_job_release_ns;
    uint64_t last_job_deadline_ns;
    uint64_t last_job_finish_ns;
    uint64_t thermal_acquired_ns;
    uint64_t cpu_window_end_ns;
    uint64_t applied_epoch;
    uint64_t applied_decision_id;
    uint32_t features;
    uint32_t ready_mask;
    uint32_t watchdog_ok;
    uint32_t cpu_valid;
    int32_t core_busy_pct[4];
    pg_thread_report placement[5];
    uint64_t worker_seen_ns[3];
    uint64_t worker_effect_ns[2];
    uint64_t first_job_id;
    uint64_t job_overwrites;
    uint64_t worker_dropped_reports;
    uint64_t enforcer_seen_ns;
    uint32_t enforcer_ok;
    uint32_t forced_off;
} pg_status_t;

#define PG_FEATURE_THREADCTL 1u
#define PG_FEATURE_ENFORCER 2u

typedef struct {
    pg_request_t request;
    pg_thread_report placement;
} pg_enforcer_request;

#define PG_JOB_BATCH 16u

typedef struct {
    uint64_t id;
    uint64_t release_ns;
    uint64_t deadline_ns;
    uint64_t finish_ns;
    uint64_t start_ns;
} pg_job_record;

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
