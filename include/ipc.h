#ifndef POWERGUARD_IPC_H
#define POWERGUARD_IPC_H
#include "metrics.h"
#include "pg_names.h"

#define PG_NAME pg_policy_name()
#define PG_VERSION 1
enum pg_message { PG_READ = 0x700, PG_SAMPLE, PG_EFFECT, PG_SAMPLE_POWER, PG_JOB, PG_TRIAL, PG_SCORE, PG_SAMPLE_BATCH,
                  PG_MANAGER_HEARTBEAT, PG_EFFECT_CONFIRMED, PG_CONFIRM_READ };
#define PG_BATCH_MAX 8
typedef struct {
    uint16_t type, version;
    uint32_t count;
    pg_sample samples[PG_BATCH_MAX];
} pg_sample_batch;
typedef struct {
    uint16_t type, version;
    uint32_t reserved;
    union {
        pg_sample sample; /* PG_SAMPLE_POWER uses reserved as unsigned index (0..100) */
        /* deadline_ns is a duration relative to release_ns (v1 contract). */
        struct { uint64_t epoch, id, release_ns, deadline_ns, finish_ns; } job;
        struct { uint64_t epoch, trigger_ns; uint32_t series, from, to; } trial;
        struct { uint64_t epoch, sequence, progress_ns; } manager;
        struct {
            uint64_t epoch, decision_id, effective_ns;
            uint32_t nc1_pct, nc2_pct;
        } effect;
    } body;
} pg_message_v1;
typedef struct {
    uint32_t version, config_generation;
    pg_status decision;
    pg_sample last_input;
} pg_reply_v1;
typedef struct {
    pg_reply_v1 snapshot;
    pg_scores scores;
    pg_spread jitter;
    uint32_t series, from, to;
} pg_score_reply;
/* Additive IPC extension: existing v1 request/reply layouts are unchanged.
 * received_ns is the FIRST accepted receipt at the supervisor, not a worker
 * timestamp or the time a later read happens to observe the acknowledgement.
 */
typedef struct {
    pg_reply_v1 snapshot;
    uint64_t detected_ns, received_ns, manager_progress_ns, manager_progress_seq;
    uint32_t restriction, reserved;
} pg_confirmation_reply;
#endif
