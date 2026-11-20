/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * include/policy.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef POWERGUARD_POLICY_H
#define POWERGUARD_POLICY_H
#include <stdint.h>

#define PG_MS UINT64_C(1000000)
#define PG_TICK (10 * PG_MS)
#define PG_TEMP_MAX_AGE (150 * PG_MS)
#define PG_CPU_MAX_AGE (150 * PG_MS)
#define PG_DWELL (2000 * PG_MS)
#define PG_RESTORE_DWELL (1000 * PG_MS)
#define PG_RECOVERY_MAX_GAP (35 * PG_MS)
#define PG_FLAT_LIMIT (2000 * PG_MS)
#define PG_LEASE (50 * PG_MS)
#define PG_MANAGER_MAX_AGE (50 * PG_MS)
#define PG_RESPONSE_TARGET (20 * PG_MS)

enum pg_state { NORMAL, WARNING, THROTTLED, CRITICAL, RECOVERY };

enum pg_reason {
    TEMP = 1,
    CPU = 2,
    POWER = 4,
    BAD_TEMP = 8,
    BAD_LOAD = 16,
    WAIT_EFFECT = 32,
    BAD_MANAGER = 64,
    FORCED_OFF = 128
};

enum pg_quality { UNAVAILABLE, GOOD, FROZEN, SENSOR_ERROR, READ_ONLY };

enum pg_source { REAL = 1, SIMULATED = 2 };

typedef struct {
    uint64_t thermal_seq;
    uint64_t acquired_ns;
    uint64_t load_end_ns;
    int32_t temp_deci_c;
    int32_t quality;
    int32_t source;
    int32_t core_busy_pct[4];
    int32_t nc_demand_pct[2];
} pg_sample;

typedef struct {
    uint64_t epoch;
    uint64_t decision_id;
    uint64_t issued_ns;
    uint64_t lease_until_ns;
    uint64_t as_of_ns;
    uint64_t effective_ns;
    uint64_t clear_since_ns;
    uint32_t state;
    uint32_t stage;
    uint32_t causes;
    uint32_t cpu_peak_pct;
    uint32_t power_index;
    uint32_t nc1_pct;
    uint32_t nc2_pct;
} pg_status;

typedef struct {
    pg_status status;
    pg_sample sample;
    uint64_t manager_seen_ns;
    uint64_t healthy_since_ns;
    uint64_t flat_since_ns;
    uint64_t restriction_from_ns;
    uint64_t manager_progress_ns;
    uint64_t manager_progress_seq;
    uint64_t restriction_decision_id;
    uint64_t effect_received_id;
    uint64_t effect_received_ns;
    uint64_t forced_off_ns;
    unsigned recovery_origin;
    int have_sample;
    int bad_sequence;
    int explicit_power;
    int power_value;
} pg_policy;

void pg_init(pg_policy *p, uint64_t now);
void pg_ingest(pg_policy *p, const pg_sample *s, uint64_t now);
void pg_ingest_power(pg_policy *p, const pg_sample *s, int index, uint64_t now);
int pg_activation_limit(unsigned signal, int value);
int pg_clear_values(int temp, int peak, int demand);
int pg_recovery_ready(const pg_policy *p);
void pg_tick(pg_policy *p, uint64_t now);
uint64_t pg_next_deadline(const pg_policy *p);

int pg_effect(pg_policy *p, uint64_t epoch, uint64_t id, uint64_t effective, unsigned nc1,
              unsigned nc2, uint64_t now);

int pg_manager_progress(pg_policy *p, uint64_t epoch, uint64_t sequence, uint64_t progress_ns,
                        uint64_t now);
#endif
