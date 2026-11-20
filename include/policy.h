#ifndef POWERGUARD_POLICY_H
#define POWERGUARD_POLICY_H
#include <stdint.h>

/* Draft v1. Local Pi monotonic nanoseconds. Percentages are integers 0..100. */
#define PG_MS UINT64_C(1000000)
#define PG_TICK (10 * PG_MS)
#define PG_TEMP_MAX_AGE (150 * PG_MS)
#define PG_CPU_MAX_AGE (150 * PG_MS)
#define PG_DWELL (2000 * PG_MS)
#define PG_RESTORE_DWELL (1000 * PG_MS)
#define PG_RECOVERY_MAX_GAP (35 * PG_MS)
#define PG_FLAT_LIMIT (2000 * PG_MS)
#define PG_LEASE (50 * PG_MS)
#define PG_RESPONSE_TARGET (20 * PG_MS) /* experimental until frozen on Pi */

enum pg_state { NORMAL, WARNING, THROTTLED, CRITICAL, RECOVERY };
enum pg_reason { TEMP = 1, CPU = 2, POWER = 4, BAD_TEMP = 8, BAD_LOAD = 16,
                 WAIT_EFFECT = 32 };
/* GOOD asserts a verified acquisition. READ_ONLY has no conversion identity. */
enum pg_quality { UNAVAILABLE, GOOD, FROZEN, SENSOR_ERROR, READ_ONLY };
enum pg_source { REAL = 1, SIMULATED = 2 };
typedef struct {
    uint64_t thermal_seq, acquired_ns, load_end_ns;
    int32_t temp_deci_c, quality, source;
    int32_t core_busy_pct[4]; /* previous sample window on each Pi core */
    int32_t nc_demand_pct[2]; /* pre-cap request, each relative to one core */
} pg_sample;
typedef struct {
    uint64_t epoch, decision_id, issued_ns, lease_until_ns, as_of_ns;
    uint64_t effective_ns, clear_since_ns;
    uint32_t state, stage, causes, cpu_peak_pct, power_index;
    uint32_t nc1_pct, nc2_pct; /* maximum fraction of each nominal budget */
} pg_status;
typedef struct {
    pg_status status;
    pg_sample sample;
    uint64_t manager_seen_ns;
    uint64_t healthy_since_ns, flat_since_ns, restriction_from_ns;
    unsigned recovery_origin;
    int have_sample, bad_sequence, explicit_power, power_value;
} pg_policy;
void pg_init(pg_policy *p, uint64_t now);
void pg_ingest(pg_policy *p, const pg_sample *s, uint64_t now);
void pg_ingest_power(pg_policy *, const pg_sample *, int power_index, uint64_t now);
int pg_activation_limit(unsigned signal, int value);
int pg_clear_values(int temp, int peak, int demand);
int pg_recovery_ready(const pg_policy *);
void pg_tick(pg_policy *p, uint64_t now);
uint64_t pg_next_deadline(const pg_policy *p);
/* The manager sends this only after BOTH limits have taken effect. */
int pg_effect(pg_policy *p, uint64_t epoch, uint64_t id, uint64_t effective,
              unsigned nc1, unsigned nc2, uint64_t now);
#endif
