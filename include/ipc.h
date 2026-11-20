#ifndef POWERGUARD_IPC_H
#define POWERGUARD_IPC_H
#include "metrics.h"

#define PG_NAME "powerguard_k_v1"
#define PG_VERSION 1
enum pg_message { PG_READ = 0x700, PG_SAMPLE, PG_EFFECT, PG_SAMPLE_POWER, PG_JOB, PG_TRIAL, PG_SCORE };
typedef struct {
    uint16_t type, version;
    uint32_t reserved;
    union {
        pg_sample sample; /* PG_SAMPLE_POWER uses reserved as unsigned index (0..100) */
        /* deadline_ns is a duration relative to release_ns (v1 contract). */
        struct { uint64_t epoch, id, release_ns, deadline_ns, finish_ns; } job;
        struct { uint64_t epoch, trigger_ns; uint32_t series, from, to; } trial;
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
#endif
