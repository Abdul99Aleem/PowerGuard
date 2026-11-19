#ifndef PG_METRICS_H
#define PG_METRICS_H
#include "policy.h"

/* One comparable trial series per case (1..8), from-state and target-state. */
#define PG_CASES 8
typedef struct {
    uint64_t count, min_ns, max_ns;
} pg_spread;
typedef struct {
    uint64_t issued, completed, late, unconfirmed, pending_id, worst_response_ns;
    uint64_t jobs, misses, job_gaps, last_job_id, last_latency_ns, max_latency_ns;
    uint64_t restores, last_restore_ns, max_restore_ns;
    uint64_t cancelled_trials;
    int32_t overshoot[3]; /* 0.1 deg C, CPU percentage points, power index points */
    uint32_t observed_mask, last_finished;
} pg_scores;
typedef struct {
    pg_scores scores;
    pg_spread trials[PG_CASES][5][5];
    uint64_t trial_ns, last_restore_id, pending_ns;
    unsigned trial_case, trial_from, trial_to;
    struct { uint64_t until_ns; int limit; } windows[3][3];
    int started_normal;
} pg_metrics;
void pg_observe(pg_metrics *, const pg_status *before, const pg_policy *);
int pg_trial(pg_metrics *, unsigned series, unsigned from, unsigned to, uint64_t start, uint64_t now);
int pg_job(pg_metrics *, uint64_t id, uint64_t release, uint64_t deadline,
           uint64_t finish, uint64_t now);
#endif
