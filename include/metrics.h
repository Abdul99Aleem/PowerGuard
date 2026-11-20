/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * include/metrics.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef PG_METRICS_H
#define PG_METRICS_H
#include "policy.h"

#define PG_CASES 8

typedef struct {
    uint64_t count;
    uint64_t min_ns;
    uint64_t max_ns;
} pg_spread;

typedef struct {
    uint64_t issued;
    uint64_t completed;
    uint64_t late;
    uint64_t unconfirmed;
    uint64_t pending_id;
    uint64_t worst_response_ns;
    uint64_t jobs;
    uint64_t misses;
    uint64_t job_gaps;
    uint64_t last_job_id;
    uint64_t last_latency_ns;
    uint64_t max_latency_ns;
    uint64_t restores;
    uint64_t last_restore_ns;
    uint64_t max_restore_ns;
    uint64_t cancelled_trials;
    int32_t overshoot[3];
    uint32_t observed_mask;
    uint32_t last_finished;
} pg_scores;

typedef struct {
    pg_scores scores;
    pg_spread trials[PG_CASES][5][5];
    uint64_t trial_ns;
    uint64_t last_restore_id;
    uint64_t pending_ns;
    unsigned trial_case;
    unsigned trial_from;
    unsigned trial_to;

    struct {
        uint64_t until_ns;
        int limit;
    } windows[3][3];

    int started_normal;
} pg_metrics;

void pg_observe(pg_metrics *m, const pg_status *old, const pg_policy *p);
int pg_trial(pg_metrics *m, unsigned series, unsigned from, unsigned to, uint64_t start,
             uint64_t now);
int pg_job(pg_metrics *m, uint64_t id, uint64_t release, uint64_t deadline, uint64_t finish,
           uint64_t now);
#endif
