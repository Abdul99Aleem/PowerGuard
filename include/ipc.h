/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * include/ipc.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef POWERGUARD_IPC_H
#define POWERGUARD_IPC_H
#include "metrics.h"
#include "pg_names.h"

#define PG_NAME pg_policy_name()
#define PG_VERSION 1

enum pg_message {
    PG_READ = 0x700,
    PG_SAMPLE,
    PG_EFFECT,
    PG_SAMPLE_POWER,
    PG_JOB,
    PG_TRIAL,
    PG_SCORE,
    PG_SAMPLE_BATCH,
    PG_MANAGER_HEARTBEAT,
    PG_EFFECT_CONFIRMED,
    PG_CONFIRM_READ
};

#define PG_BATCH_MAX 8

typedef struct {
    uint16_t type;
    uint16_t version;
    uint32_t count;
    pg_sample samples[PG_BATCH_MAX];
} pg_sample_batch;

typedef struct {
    uint16_t type;
    uint16_t version;
    uint32_t reserved;

    union {
        pg_sample sample;

        struct {
            uint64_t epoch;
            uint64_t id;
            uint64_t release_ns;
            uint64_t deadline_ns;
            uint64_t finish_ns;
        } job;

        struct {
            uint64_t epoch;
            uint64_t trigger_ns;
            uint32_t series;
            uint32_t from;
            uint32_t to;
        } trial;

        struct {
            uint64_t epoch;
            uint64_t sequence;
            uint64_t progress_ns;
        } manager;

        struct {
            uint64_t epoch;
            uint64_t decision_id;
            uint64_t effective_ns;
            uint32_t nc1_pct;
            uint32_t nc2_pct;
        } effect;
    } body;
} pg_message_v1;

typedef struct {
    uint32_t version;
    uint32_t config_generation;
    pg_status decision;
    pg_sample last_input;
} pg_reply_v1;

typedef struct {
    pg_reply_v1 snapshot;
    pg_scores scores;
    pg_spread jitter;
    uint32_t series;
    uint32_t from;
    uint32_t to;
} pg_score_reply;

typedef struct {
    pg_reply_v1 snapshot;
    uint64_t detected_ns;
    uint64_t received_ns;
    uint64_t manager_progress_ns;
    uint64_t manager_progress_seq;
    uint32_t restriction;
    uint32_t reserved;
} pg_confirmation_reply;
#endif
