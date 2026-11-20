#ifndef POWERGUARD_POLICY_H
#define POWERGUARD_POLICY_H
#include <stdint.h>

/* Draft v1. Local Pi monotonic nanoseconds. Percentages are integers 0..100. */
#define PG_MS UINT64_C(1000000)
#define PG_TICK (10 * PG_MS)
#define PG_TEMP_MAX_AGE (150 * PG_MS) /* no valid acquisition for this long: Critical */
#define PG_CPU_MAX_AGE (50 * PG_MS)
#define PG_LEASE (50 * PG_MS)
#define PG_RESPONSE_TARGET (20 * PG_MS) /* experimental until frozen on Pi */

/* Continuous all-clear time before capacity returns (PRD page 7). */
#define PG_WARNING_CLEAR (2000 * PG_MS) /* Warning straight back to Normal */
#define PG_RESTORE_NC1 (2000 * PG_MS)   /* recovery: NC1 reopens */
#define PG_RESTORE_NC2 (1000 * PG_MS)   /* then NC2, after NC1 is confirmed */
#define PG_RECOVERY_GAP (35 * PG_MS)    /* a longer sampling gap breaks the dwell */

/* An unverified raw value that stays exactly the same this long cannot be
   told apart from a stuck sensor (PRD page 4). */
#define PG_FLAT_LIMIT (2000 * PG_MS)

/* Limits for each input (PRD page 7). Temperature is in tenths of a degree C,
   U is the busiest core in percent and P is the 0..100 demand index.
   Reaching an activation limit triggers it. Recovery time only accrues while
   every input is at or below its clear level. */
#define PG_TEMP_CLEAR 620
#define PG_TEMP_WARN 650
#define PG_TEMP_THROTTLE 700
#define PG_TEMP_CRITICAL 750
#define PG_CPU_CLEAR 55
#define PG_CPU_WARN 70
#define PG_CPU_THROTTLE 85
#define PG_CPU_CRITICAL 95
#define PG_POWER_CLEAR 45
#define PG_POWER_WARN 60
#define PG_POWER_THROTTLE 75
#define PG_POWER_CRITICAL 90

enum pg_state { NORMAL, WARNING, THROTTLED, CRITICAL, RECOVERY };
/* Worker bits for confirmed and unknown. */
#define PG_NC1 1u
#define PG_NC2 2u
#define PG_NC_BOTH (PG_NC1 | PG_NC2)
#define PG_CAP_UNKNOWN UINT32_MAX /* no trusted confirmation in this epoch */
enum pg_reason { TEMP = 1, CPU = 2, POWER = 4, BAD_TEMP = 8, BAD_LOAD = 16,
                 WAIT_EFFECT = 32, TEMP_EXPIRED = 64, UNVERIFIED_FLAT = 128 };
enum pg_quality { UNAVAILABLE, GOOD, FROZEN, SENSOR_ERROR };
enum pg_source { REAL = 1, SIMULATED = 2 };
typedef struct {
    uint64_t thermal_seq, acquired_ns, load_end_ns;
    int32_t temp_deci_c, quality, source;
    int32_t core_busy_pct[4]; /* previous sample window on each Pi core */
    int32_t nc_demand_pct[2]; /* pre-cap request, each relative to one core */
    int32_t verified; /* 1 when the source proves each reading is a new conversion */
} pg_sample;
typedef struct {
    uint64_t epoch, decision_id, issued_ns, lease_until_ns, as_of_ns;
    uint64_t effective_ns, clear_since_ns;
    uint64_t applied_ns[2];   /* when NC1 and NC2 applied the current decision */
    uint32_t applied_pct[2];  /* last cap each worker confirmed, or PG_CAP_UNKNOWN */
    uint32_t confirmed;       /* workers that confirmed the current decision */
    uint32_t unknown;         /* workers whose effect missed its deadline */
    uint32_t decision_causes; /* causes when the current decision was issued */
    uint32_t state, causes, cpu_peak_pct, power_index;
    uint32_t stage; /* cap level being applied, numbered like the states */
    uint32_t nc1_pct, nc2_pct; /* maximum fraction of each nominal budget */
} pg_status;
typedef struct {
    pg_status status;
    pg_sample sample;
    uint64_t last_eval_ns, manager_seen_ns;
    uint64_t healthy_since_ns;
    uint64_t nc1_step_ns; /* when this recovery passed its NC1 step */
    uint64_t last_good_ns; /* acquisition time of the latest valid reading */
    uint64_t expired_at_ns; /* latest age deadline that passed */
    uint64_t flat_since_ns; /* first acquisition of the current raw value */
    unsigned recovery_from; /* Throttled or Critical, restored on a cancel */
    int have_sample, bad_sequence;
} pg_policy;
void pg_init(pg_policy *p, uint64_t now);
void pg_ingest(pg_policy *p, const pg_sample *s, uint64_t now);
int pg_activation_limit(unsigned signal, int value);
int pg_recovery_ready(const pg_policy *);
void pg_tick(pg_policy *p, uint64_t now);
/* Absolute time the temperature expires without a newer valid reading, or 0. */
uint64_t pg_age_deadline(const pg_policy *p);
/* One worker applied the current decision. nc is 1 or 2. */
int pg_worker_effect(pg_policy *p, uint64_t epoch, uint64_t id, unsigned nc,
                     unsigned cap, uint64_t applied, uint64_t now);
/* Both workers applied the current decision at the same time. */
int pg_effect(pg_policy *p, uint64_t epoch, uint64_t id, uint64_t effective,
              unsigned nc1, unsigned nc2, uint64_t now);
#endif
