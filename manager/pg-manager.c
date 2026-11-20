/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * manager/pg-manager.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE 1
#include "bridge.h"
#include "ipc.h"
#include "pg_threadctl.h"
#include <errno.h>
#if defined(PG_BENCH_TRIAL)
#include <inttypes.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>
#if defined(PG_BENCH_TRIAL)
#include ".milestone-cpu-meter.inc"
#endif

static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
    (void)sig;
    running = 0;
}

static int timed_send(int coid, const void *request, size_t bytes, void *reply, size_t capacity)
{
    uint64_t timeout = PG_RESPONSE_TARGET;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout,
                     NULL) == -1) {
        return 0;
    }
    return MsgSend(coid, request, bytes, reply, capacity) != -1;
}

static int bench_send(int coid, unsigned command, int arg, int extra, pg_status_t *reply)
{
    pg_request_t request = {PG_BENCH_TYPE, PG_BENCH_VERSION, command, arg, extra};
    if (!timed_send(coid, &request, sizeof(request), reply, sizeof(*reply))) {
        return 0;
    }
    if (reply->error) {
        errno = reply->error;
        return 0;
    }
    return 1;
}

static int policy_send(int coid, const pg_message_v1 *request, pg_reply_v1 *reply)
{
    return timed_send(coid, request, sizeof(*request), reply, sizeof(*reply));
}

static int resume_cursor(int coid, uint64_t *epoch, uint64_t *job, uint64_t *sequence)
{
    pg_message_v1 q = {.type = PG_SCORE, .version = PG_VERSION};
    q.body.trial.series = 1;
    q.body.trial.from = NORMAL;
    q.body.trial.to = WARNING;
    pg_score_reply score;
    pg_confirmation_reply progress;
    if (!timed_send(coid, &q, sizeof(q), &score, sizeof(score))) {
        return 0;
    }
    q.type = PG_CONFIRM_READ;
    if (!timed_send(coid, &q, sizeof(q), &progress, sizeof(progress))) {
        return 0;
    }
    if (score.snapshot.version != PG_VERSION || progress.snapshot.version != PG_VERSION ||
        score.snapshot.decision.epoch != progress.snapshot.decision.epoch) {
        errno = EPROTO;
        return 0;
    }
    *epoch = score.snapshot.decision.epoch;
    *job = score.scores.last_job_id;
    *sequence = progress.manager_progress_seq;
    return 1;
}

static int clamp_percent(int value)
{
    return value < 0 ? 0 : value > 100 ? 100 : value;
}

static int retry_policy(void)
{
    return running && (errno == ETIMEDOUT || errno == EINTR);
}

int main(void)
{
    int result = 0;
#if defined(PG_BENCH_TRIAL)
#if defined(OVERHEAD_TRIAL)
    const int measure_cores = 0;
#else
    const int measure_cores = getenv("PG_T3_CPU_SPIKE") != NULL;
#endif
    if (measure_cores && !find_idle_clocks()) {
        return 1;
    }
    uint64_t effect_epoch = 0;
    uint64_t effect_id = 0;
    uint64_t effect_reply_ns = 0;
#endif
    pg_thread_report placement;
    if (pg_thread_configure(0, 20, "pg-manager", &placement)) {
        perror("manager ThreadCtl/scheduling qualification");
        return 1;
    }
    pg_thread_print(&placement);
    int policy_coid = name_open(PG_NAME, 0);
    if (policy_coid == -1) {
        perror("name_open PowerGuard");
        return 1;
    }
    int bench_coid = name_open(PG_BENCH_NAME, 0);
    if (bench_coid == -1) {
        perror("name_open pgbench");
        name_close(policy_coid);
        return 1;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    uint64_t last_bench_job = 0;
    uint64_t policy_epoch = 0;
    uint64_t progress_sequence = 0;
    if (!resume_cursor(policy_coid, &policy_epoch, &last_bench_job, &progress_sequence)) {
        perror("manager accepted cursor");
        return 1;
    }
    int last_state = PG_ST_UNKNOWN;
    int last_recovery = -1;
    int need_resume = 0;
    puts(
        "pg-manager connected: telemetry/jobs/progress; split runtime uses independent enforcement");
    while (running) {
        int waiting_for_critical = 0;
        pg_status_t bench;
        if (!bench_send(bench_coid, PG_CMD_STATUS, 0, 0, &bench)) {
            perror("pgbench status");
            result = 1;
            break;
        }
        int split_runtime = (bench.features & PG_FEATURE_THREADCTL) != 0;
        int cpu = clamp_percent(bench.process_cpu_per_core_tenths / 10);
#if defined(PG_BENCH_TRIAL)
        int demand2 = bench.scenario == PG_SC_SPIKE ? (measure_cores ? 93 : 90) : 15;
#else
        int demand2 = bench.scenario == PG_SC_SPIKE ? 90 : 15;
#endif
        pg_sample sample;
        memset(&sample, 0, sizeof(sample));
        sample.thermal_seq = bench.thermal_acquired_ns;
        sample.acquired_ns = bench.thermal_acquired_ns;
        sample.load_end_ns = bench.cpu_window_end_ns;
        sample.temp_deci_c = bench.temperature_milli_c / 100;
        sample.quality =
            split_runtime && (bench.ready_mask != 7 || !bench.watchdog_ok || !bench.cpu_valid ||
                              ((bench.features & PG_FEATURE_ENFORCER) && !bench.enforcer_ok))
                ? SENSOR_ERROR
                : GOOD;
        sample.source = SIMULATED;
        for (unsigned i = 0; i < 4; ++i) {
            sample.core_busy_pct[i] = split_runtime ? bench.core_busy_pct[i] : cpu;
        }
#if defined(PG_BENCH_TRIAL)
        if (measure_cores) {
            int measured = read_core_load(&sample);
            if (measured < 0) {
                fputs("Could not read per-core idle time.\n", stderr);
                result = 1;
                break;
            }
            if (!measured) {
                struct timespec warmup = {.tv_nsec = PG_TICK};
                nanosleep(&warmup, NULL);
                continue;
            }
        }
#endif
        sample.nc_demand_pct[0] = 20;
        sample.nc_demand_pct[1] = demand2;

        pg_message_v1 msg;
        memset(&msg, 0, sizeof(msg));
#if defined(OVERHEAD_TRIAL)
        msg.type = PG_READ;
#else
        msg.type = PG_SAMPLE;
#endif
        msg.version = PG_VERSION;
        msg.body.sample = sample;
        pg_reply_v1 decision;
        if (!policy_send(policy_coid, &msg, &decision)) {
            if (retry_policy()) {
                need_resume = 1;
                goto pause_loop;
            }
            perror("PowerGuard sample");
            result = 1;
            break;
        }

        if (need_resume || decision.decision.epoch != policy_epoch) {
            if (!resume_cursor(policy_coid, &policy_epoch, &last_bench_job, &progress_sequence)) {
                if (retry_policy()) {
                    need_resume = 1;
                    goto pause_loop;
                }
                perror("manager new-epoch cursor");
                result = 1;
                break;
            }
            need_resume = 0;
        }
        if (last_bench_job > bench.last_job_id) {
            fputs("Workload release IDs restarted without a new run; qualification failed\n",
                  stderr);
            result = 1;
            break;
        }

        if (!(bench.features & PG_FEATURE_ENFORCER)) {
            const pg_status *want = &decision.decision;
#if defined(PG_BENCH_TRIAL)
            if (measure_cores && bench.scenario == PG_SC_SPIKE) {
                record_spike(want, &sample);
            }
#endif
            pg_cap_request_t grant = {.request = {PG_BENCH_TYPE, PG_BENCH_VERSION,
                                                  PG_CMD_LEASED_CAPS, (int)want->nc1_pct,
                                                  (int)want->nc2_pct},
                                      .epoch = want->epoch,
                                      .decision_id = want->decision_id,
                                      .until_ns = want->lease_until_ns};
            if (!timed_send(bench_coid, &grant, sizeof(grant), &bench, sizeof(bench)) ||
                (bench.error && bench.error != ESTALE)) {
                fputs("pgbench cap delivery failed\n", stderr);
                result = 1;
                break;
            }
            int grant_accepted = bench.error == 0;
            int recovery = want->state == RECOVERY ? (int)want->nc1_pct : 0;
            int state = pg_bench_state_from_policy((enum pg_state)want->state);
            if (state != last_state || recovery != last_recovery) {
                if (!bench_send(bench_coid, PG_CMD_STATE, state, recovery, &bench)) {
                    perror("pgbench state");
                    result = 1;
                    break;
                }
                last_state = state;
                last_recovery = recovery;
            }
            int caps_applied = grant_accepted && bench.applied_epoch == want->epoch &&
                               bench.applied_decision_id == want->decision_id &&
                               bench.nc1_cap_pct == (int)want->nc1_pct &&
                               bench.nc2_cap_pct == (int)want->nc2_pct &&
                               bench.last_effective_ns >= want->issued_ns;
            waiting_for_critical = grant_accepted && want->state == CRITICAL && !caps_applied;
            if (caps_applied) {
                memset(&msg, 0, sizeof(msg));
                msg.type = PG_EFFECT;
                msg.version = PG_VERSION;
                msg.body.effect.epoch = want->epoch;
                msg.body.effect.decision_id = want->decision_id;
                msg.body.effect.effective_ns = bench.last_effective_ns;
                msg.body.effect.nc1_pct = want->nc1_pct;
                msg.body.effect.nc2_pct = want->nc2_pct;
#if defined(PG_BENCH_TRIAL)
                int record = want->state == CRITICAL && !msg.body.effect.nc1_pct &&
                             !msg.body.effect.nc2_pct &&
                             (effect_epoch != msg.body.effect.epoch ||
                              effect_id != msg.body.effect.decision_id);
                if (!policy_send(policy_coid, &msg, &decision)) {
                    if (errno != EINVAL) {
                        perror("PowerGuard effect");
                        result = 1;
                        break;
                    }
                } else if (record) {
                    struct timespec now;
                    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                        effect_epoch = msg.body.effect.epoch;
                        effect_id = msg.body.effect.decision_id;
                        effect_reply_ns =
                            (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
                    }
                }
#else
                if (!policy_send(policy_coid, &msg, &decision) && errno != EINVAL) {
                    perror("PowerGuard effect");
                    result = 1;
                    break;
                }
#endif
            }
        }

        if (split_runtime) {
            pg_jobs_request query = {
                .request = {PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_JOBS, 0, 0},
                .after_id = last_bench_job};
            pg_jobs_reply batch;
            if (!timed_send(bench_coid, &query, sizeof(query), &batch, sizeof(batch)) ||
                batch.error) {
                fputs("Critical job stream incomplete/overwritten; qualification failed\n", stderr);
                result = 1;
                break;
            }
            for (unsigned i = 0; i < batch.count; ++i) {
                const pg_job_record *job = &batch.jobs[i];
                if (job->id != last_bench_job + 1) {
                    fputs("Critical job record gap; qualification failed\n", stderr);
                    result = 1;
                    break;
                }
                memset(&msg, 0, sizeof(msg));
                msg.type = PG_JOB;
                msg.version = PG_VERSION;
                msg.body.job.epoch = decision.decision.epoch;
                msg.body.job.id = job->id;
                msg.body.job.release_ns = job->release_ns;
                msg.body.job.deadline_ns = pg_job_budget_ns(job->release_ns, job->deadline_ns);
                msg.body.job.finish_ns = job->finish_ns;
                if (!policy_send(policy_coid, &msg, &decision)) {
                    if (retry_policy()) {
                        need_resume = 1;
                        goto pause_loop;
                    }
                    perror("PowerGuard job stream");
                    result = 1;
                    break;
                }
                last_bench_job = job->id;
            }
            if (result) {
                break;
            }
        } else if (bench.last_job_id > last_bench_job) {
            memset(&msg, 0, sizeof(msg));
            msg.type = PG_JOB;
            msg.version = PG_VERSION;
            msg.body.job.epoch = decision.decision.epoch;
            msg.body.job.id = bench.last_job_id;
            msg.body.job.release_ns = bench.last_job_release_ns;
            msg.body.job.deadline_ns =
                pg_job_budget_ns(bench.last_job_release_ns, bench.last_job_deadline_ns);
            msg.body.job.finish_ns = bench.last_job_finish_ns;
            if (!policy_send(policy_coid, &msg, &decision)) {
                if (retry_policy()) {
                    need_resume = 1;
                    goto pause_loop;
                }
                perror("PowerGuard job");
                result = 1;
                break;
            }
            last_bench_job = bench.last_job_id;
        }

        if (bench.features & PG_FEATURE_ENFORCER) {
            struct timespec progress;
            if (clock_gettime(CLOCK_MONOTONIC, &progress)) {
                perror("manager progress clock");
                result = 1;
                break;
            }
            memset(&msg, 0, sizeof(msg));
            msg.type = PG_MANAGER_HEARTBEAT;
            msg.version = PG_VERSION;
            msg.body.manager.epoch = decision.decision.epoch;
            msg.body.manager.sequence = ++progress_sequence;
            msg.body.manager.progress_ns =
                (uint64_t)progress.tv_sec * UINT64_C(1000000000) + progress.tv_nsec;
            if (!policy_send(policy_coid, &msg, &decision)) {
                if (retry_policy()) {
                    need_resume = 1;
                    goto pause_loop;
                }
                perror("manager progress heartbeat");
                result = 1;
                break;
            }
        }
    pause_loop:;

#if defined(PG_BENCH_TRIAL)
        struct timespec pause = {.tv_nsec = waiting_for_critical ? PG_MS : PG_TICK};
#else
        struct timespec pause = {.tv_nsec = waiting_for_critical ? PG_MS : 5 * PG_MS};
#endif
        uint64_t tolerance = 1;
        if (TimerTimeout(CLOCK_MONOTONIC, TIMER_TOLERANCE, NULL, &tolerance, NULL) == -1) {
            perror("manager high resolution tolerance");
            result = 1;
            break;
        }
        while (running && nanosleep(&pause, &pause) == -1 && errno == EINTR) {
        }
    }
#if defined(PG_BENCH_TRIAL)
    if (measure_cores) {
        export_spike();
    }
    if (effect_reply_ns) {
        FILE *evidence = fopen("manager-confirmation.csv", "w");
        if (!evidence) {
            perror("manager-confirmation.csv");
        } else {
            fprintf(evidence, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", effect_epoch, effect_id,
                    effect_reply_ns);
            if (fclose(evidence)) {
                perror("manager-confirmation.csv close");
            }
        }
    }
#endif
    name_close(bench_coid);
    name_close(policy_coid);
    return result;
}
