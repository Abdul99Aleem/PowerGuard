#ifndef PG_BENCH_PROTOCOL_H
#define PG_BENCH_PROTOCOL_H

#include <stdint.h>

/*
 * Minimal local QNX name_attach/name_open + MsgSend contract.
 * A supervisor can send PG_CMD_LEVEL and PG_CMD_STATE messages.
 * All timestamps are CLOCK_MONOTONIC, all timing figures are measured.
 */
#define PG_BENCH_NAME "powerguard_bench"
#define PG_BENCH_TYPE 0x2001u
#define PG_BENCH_VERSION 1u

enum pg_command {
    PG_CMD_STATUS = 1,
    PG_CMD_SCENARIO = 2,
    PG_CMD_LEVEL = 3,
    PG_CMD_STATE = 4,
    PG_CMD_STOP = 5
};

enum pg_scenario {
    PG_SC_NORMAL = 0,
    PG_SC_RAMP = 1,
    PG_SC_SPIKE = 2,
    PG_SC_FREEZE = 3
};

/* These are REPORTS from the supervisor, not states decided by this testbench. */
enum pg_state {
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
    int32_t error;                /* 0 = success, otherwise POSIX errno */
    int32_t scenario;
    int32_t level_requested;
    int32_t level_applied;
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
    uint64_t detection_to_actuation_us; /* 0 when no qualifying detection */
    uint64_t recovery_to_full_ms; /* 0 when not measured */
} pg_status_t;

#endif
