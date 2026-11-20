/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * testbench/pgbench.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#define _POSIX_C_SOURCE 200809L
#include "pgbench_protocol.h"
#include "pg_progress.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/select.h>

#define NS 1000000000ULL
#define PERIOD_NS 10000000ULL
#define DEADLINE_NS 5000000ULL
#define JOB_NS 1000000ULL
#define WORKER_NS 10000000ULL
#define WORK_CHUNK_NS 500000ULL
#define TELEMETRY_NS 10000000ULL
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cap_changed;
static pg_progress *progress;
static int force_applied;
static volatile sig_atomic_t running = 1;
static pg_status_t st;
static uint64_t start_ns;
static uint64_t last_sensor_ns;
static uint64_t last_tick_ns;
static uint64_t last_cpu_ns;
static int level = 2;
static int scenario = PG_SC_NORMAL;
static int current_state = PG_ST_UNKNOWN;
static int nc1_cap;
static int nc2_cap;
static int stop_nc1_after_nc2;
static uint64_t cap_epoch;
static uint64_t cap_id;
static uint64_t lease_until_ns;
static uint64_t level_command_ns;
static uint64_t ack_ns[2];
static uint64_t recover_ns;
static uint64_t scenario_started_ns;

static uint64_t now_ns(clockid_t id)
{
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * NS + (uint64_t)t.tv_nsec;
}

static int init_cap_condition(void)
{
    pthread_condattr_t attr;
    int error = pthread_condattr_init(&attr);
    if (error) {
        return error;
    }
    error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!error) {
        error = pthread_cond_init(&cap_changed, &attr);
    }
    pthread_condattr_destroy(&attr);
    return error;
}

static void sleep_until(uint64_t when)
{
    struct timespec t = {(time_t)(when / NS), (long)(when % NS)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {
    }
}

static void burn_until(uint64_t until)
{
    volatile unsigned long x = 1;
    while (now_ns(CLOCK_MONOTONIC) < until) {
        x = x * 1664525UL + 1013904223UL;
    }
    (void)x;
}
#if defined(PG_BENCH_TRIAL)
static struct {
    uint64_t release;
    uint64_t start;
    uint64_t finish;
} milestone_jobs[10000];

static size_t milestone_count;
static unsigned milestone_dropped;

static void record_job(uint64_t release, uint64_t start, uint64_t finish)
{
    if (milestone_count == sizeof milestone_jobs / sizeof milestone_jobs[0]) {
        ++milestone_dropped;
        return;
    }
    size_t i = milestone_count++;
    milestone_jobs[i].release = release;
    milestone_jobs[i].start = start;
    milestone_jobs[i].finish = finish;
}

static void export_jobs(void)
{
    FILE *f = fopen("critical-jobs.csv", "w");
    if (!f) {
        perror("critical-jobs.csv");
        return;
    }
    fputs("release_ns,start_ns,finish_ns\n", f);
    for (size_t i = 0; i < milestone_count; ++i) {
        fprintf(f, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", milestone_jobs[i].release,
                milestone_jobs[i].start, milestone_jobs[i].finish);
    }
    if (fclose(f)) {
        perror("critical-jobs.csv close");
    }
    printf("Critical local records=%zu buffer_overflow=%u\n", milestone_count, milestone_dropped);
}

static void pin_spike_thread(const char *role, unsigned cpu)
{
    if (!getenv("PG_T3_CPU_SPIKE")) {
        return;
    }
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) {
        perror("apply CPU spike affinity");
        exit(1);
    }
    printf("CPU placement: %s on CPU%u.\n", role, cpu);
    fflush(stdout);
}
#endif
static void *critical_worker(void *unused)
{
#if defined(PG_BENCH_TRIAL)
    pin_spike_thread("critical", 0);
#endif
    (void)unused;
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = 20;

    int fifo_ok = (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0);
    pthread_mutex_lock(&mu);
    st.critical_fifo_ok = fifo_ok;
    pthread_mutex_unlock(&mu);
    uint64_t release = now_ns(CLOCK_MONOTONIC) + PERIOD_NS;
    while (running) {
        sleep_until(release);
        if (!running) {
            break;
        }
        uint64_t began = now_ns(CLOCK_MONOTONIC);
        burn_until(now_ns(CLOCK_MONOTONIC) + JOB_NS);
        uint64_t ended = now_ns(CLOCK_MONOTONIC);
#if defined(PG_BENCH_TRIAL)
        record_job(release, began, ended);
#endif
        uint64_t response = ended > release ? ended - release : 0;
        uint64_t jitter = began > release ? began - release : release - began;
        pthread_mutex_lock(&mu);
        st.critical_jobs++;
        st.last_job_id = st.critical_jobs;
        st.last_job_release_ns = release;
        st.last_job_deadline_ns = release + DEADLINE_NS;
        st.last_job_finish_ns = ended;
        if (ended > release + DEADLINE_NS) {
            st.critical_misses++;
        }
        if (response / 1000 > st.critical_max_response_us) {
            st.critical_max_response_us = response / 1000;
        }
        if (jitter / 1000 > st.critical_max_jitter_us) {
            st.critical_max_jitter_us = jitter / 1000;
        }
        release += PERIOD_NS;
        if (release < ended) {
            uint64_t skipped = (ended - release + PERIOD_NS - 1) / PERIOD_NS;
#if defined(PG_BENCH_TRIAL)
            for (uint64_t j = 0; j < skipped; ++j) {
                record_job(release + j * PERIOD_NS, 0, 0);
            }
#endif
            st.critical_jobs += skipped;
            st.critical_misses += skipped;
            release += skipped * PERIOD_NS;
        }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}
#if defined(PG_BENCH_TRIAL)
static struct {
    uint64_t epoch;
    uint64_t decision;
    uint64_t time;
    int worker;
    int cap;
} milestone_effects[256];

static size_t milestone_effect_count;
static uint64_t milestone_expiry_ns;
static uint64_t milestone_expired_at_ns;
static uint64_t ftti_start_ns;
static uint64_t ftti_first_ns;
static uint64_t ftti_80_ns;
static int32_t ftti_first_temp;
static unsigned milestone_effect_dropped;

static void record_effect(int worker, int cap, uint64_t epoch, uint64_t decision, uint64_t now)
{
    if (milestone_effect_count == sizeof milestone_effects / sizeof milestone_effects[0]) {
        ++milestone_effect_dropped;
        return;
    }
    size_t i = milestone_effect_count++;
    milestone_effects[i].epoch = epoch;
    milestone_effects[i].decision = decision;
    milestone_effects[i].time = now;
    milestone_effects[i].worker = worker;
    milestone_effects[i].cap = cap;
}

static void export_effects(void)
{
    if (ftti_start_ns) {
        FILE *ramp = fopen("ftti-ramp.csv", "w");
        if (!ramp) {
            perror("ftti-ramp.csv");
        } else {
            fprintf(ramp, "%" PRIu64 ",%" PRIu64 ",%" PRId32 ",%" PRIu64 "\n", ftti_start_ns,
                    ftti_first_ns, ftti_first_temp, ftti_80_ns);
            if (fclose(ramp)) {
                perror("ftti-ramp.csv close");
            }
        }
    }
    if (milestone_expiry_ns) {
        FILE *lease = fopen("lease-expiry.csv", "w");
        if (!lease) {
            perror("lease-expiry.csv");
        } else {
            fprintf(lease, "%" PRIu64 ",%" PRIu64 "\n", milestone_expiry_ns,
                    milestone_expired_at_ns);
            if (fclose(lease)) {
                perror("lease-expiry.csv close");
            }
        }
    }
    FILE *f = fopen("worker-effects.csv", "w");
    if (!f) {
        perror("worker-effects.csv");
        return;
    }
    fputs("epoch,decision,worker,cap_pct,effective_ns\n", f);
    for (size_t i = 0; i < milestone_effect_count; ++i) {
        fprintf(f, "%" PRIu64 ",%" PRIu64 ",%d,%d,%" PRIu64 "\n", milestone_effects[i].epoch,
                milestone_effects[i].decision, milestone_effects[i].worker,
                milestone_effects[i].cap, milestone_effects[i].time);
    }
    if (fclose(f)) {
        perror("worker-effects.csv close");
    }
    printf("Worker effect records=%zu buffer_overflow=%u\n", milestone_effect_count,
           milestone_effect_dropped);
}

#endif

static int worker_cap(int id, uint64_t now)
{
    if (pg_progress_forced(progress) && !force_applied) {
        force_applied = 1;
        st.forced_off = 1;
        stop_nc1_after_nc2 = nc1_cap && nc2_cap;
        if (!stop_nc1_after_nc2) {
            nc1_cap = 0;
        }
        nc2_cap = 0;
        lease_until_ns = 0;
        level = st.level_requested = 2;
        level_command_ns = now;
        ack_ns[0] = ack_ns[1] = 0;
        pthread_cond_broadcast(&cap_changed);
    }

    if (force_applied) {
        nc2_cap = 0;
        if (!stop_nc1_after_nc2) {
            nc1_cap = 0;
        }
    }
    if (lease_until_ns && now >= lease_until_ns) {
#if defined(PG_BENCH_TRIAL)
        milestone_expiry_ns = lease_until_ns;
        milestone_expired_at_ns = now;
#endif

        nc2_cap = nc1_cap = 0;
        lease_until_ns = 0;
        stop_nc1_after_nc2 = 0;
        level = st.level_requested = 2;
        level_command_ns = now;
        ack_ns[0] = ack_ns[1] = 0;
        pthread_cond_broadcast(&cap_changed);
    }

    if (stop_nc1_after_nc2 && (id == 1 || now - level_command_ns >= WORKER_NS)) {
        nc1_cap = 0;
        stop_nc1_after_nc2 = 0;
        pthread_cond_broadcast(&cap_changed);
    }
    int cap = id == 0 ? nc1_cap : nc2_cap;
    if (!(id == 0 && stop_nc1_after_nc2) && ack_ns[id] < level_command_ns) {
        ack_ns[id] = now;
#if defined(PG_BENCH_TRIAL)
        record_effect(id + 1, cap, cap_epoch, lease_until_ns ? cap_id : 0, now);
#endif
        if (ack_ns[0] >= level_command_ns && ack_ns[1] >= level_command_ns) {
            uint64_t latest = ack_ns[0] > ack_ns[1] ? ack_ns[0] : ack_ns[1];
            st.last_actuation_us = (latest - level_command_ns) / 1000;
            st.level_applied = level;
            st.nc1_cap_pct = nc1_cap;
            st.nc2_cap_pct = nc2_cap;
            st.last_effective_ns = latest;
            st.applied_epoch = cap_epoch;
            st.applied_decision_id = lease_until_ns ? cap_id : 0;
            if (level == 0 && recover_ns != 0) {
                st.recovery_to_full_ms = (latest - recover_ns) / 1000000;
                recover_ns = 0;
            }
        }
    }
    return cap;
}

#if defined(PG_BENCH_TRIAL)

static int worker_fault;
#endif
static void *noncritical_worker(void *ptr)
{
    int id = (int)(intptr_t)ptr;
#if defined(PG_BENCH_TRIAL)
    pin_spike_thread(id == 0 ? "NC1" : "NC2", id == 0 ? 2 : 3);
    int spike_duty = getenv("PG_T3_CPU_SPIKE") ? 93 : 90;
#endif
    uint64_t frame = now_ns(CLOCK_MONOTONIC);
    pthread_mutex_lock(&mu);
    while (running) {
        uint64_t now = now_ns(CLOCK_MONOTONIC);
        frame += (now - frame) / WORKER_NS * WORKER_NS;
#if defined(PG_BENCH_TRIAL)
        if (id == 1 && worker_fault && level == 2 && st.level_applied == 0 &&
            ack_ns[1] < level_command_ns) {
            int fault = worker_fault;
            worker_fault = 0;
            pthread_mutex_unlock(&mu);
            if (fault == 2) {
                return NULL;
            }
            sleep_until(now + 40000000ULL);
            pthread_mutex_lock(&mu);
            continue;
        }
#endif
        int cap = worker_cap(id, now);

#if defined(PG_BENCH_TRIAL)
        int percent = id == 0 ? 20 : (scenario == PG_SC_SPIKE ? spike_duty : 15);
#else
        int percent = id == 0 ? 20 : (scenario == PG_SC_SPIKE ? 90 : 15);
#endif
        uint64_t wake = frame + WORKER_NS;
        if (lease_until_ns && lease_until_ns < wake) {
            wake = lease_until_ns;
        }
        if (id == 0 && stop_nc1_after_nc2 && level_command_ns + WORKER_NS < wake) {
            wake = level_command_ns + WORKER_NS;
        }
        uint64_t work_end = frame + (uint64_t)percent * (unsigned)cap * WORKER_NS / 10000;
        if (work_end > wake) {
            work_end = wake;
        }
        if (work_end > now) {
            uint64_t chunk_end = now + WORK_CHUNK_NS;
            pthread_mutex_unlock(&mu);
            burn_until(work_end < chunk_end ? work_end : chunk_end);
            pthread_mutex_lock(&mu);
            continue;
        }
        struct timespec until = {(time_t)(wake / NS), (long)(wake % NS)};
        int error = pthread_cond_timedwait(&cap_changed, &mu, &until);
        if (error && error != ETIMEDOUT) {
            pthread_mutex_unlock(&mu);
            fprintf(stderr, "NC%d cap wait: %s\n", id + 1, strerror(error));
            return NULL;
        }
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}
#if defined(OVERHEAD_TRIAL)
static void *sampler(void *unused)
{
    (void)unused;
    return NULL;
}
#else
static int32_t ramp_temperature(uint64_t elapsed, uint64_t n)
{
#if defined(PG_BENCH_TRIAL)
    if (ftti_start_ns) {
        int32_t temp = elapsed >= NS / 4 ? 80000 : 75000 + (int32_t)(elapsed / 50000);
        if (!ftti_first_ns) {
            ftti_first_ns = n;
            ftti_first_temp = temp;
        }
        if (temp == 80000 && !ftti_80_ns) {
            ftti_80_ns = n;
        }
        return temp;
    }
#else
    (void)n;
#endif
    const uint64_t seconds = elapsed / NS;
    return 45000 + (int32_t)(seconds > 35 ? 35000 : seconds * 1000);
}

static void *sampler(void *unused)
{
    (void)unused;
    while (running) {
        sleep_until(now_ns(CLOCK_MONOTONIC) + TELEMETRY_NS);
        uint64_t n = now_ns(CLOCK_MONOTONIC);
        uint64_t cpu = now_ns(CLOCK_PROCESS_CPUTIME_ID);
        pthread_mutex_lock(&mu);

        uint64_t elapsed = n - scenario_started_ns;
        if (scenario != PG_SC_FREEZE || elapsed < 4 * NS) {
            int32_t temp = 45000;
            if (scenario == PG_SC_RAMP) {
                temp = ramp_temperature(elapsed, n);
            } else if (scenario == PG_SC_SPIKE) {
                temp = 56000;
            } else if (scenario == PG_SC_FREEZE) {
                temp = 52000;
            }
            st.temperature_milli_c = temp;
            last_sensor_ns = n;
        }
        if (last_tick_ns && n > last_tick_ns) {
            uint64_t delta_cpu = cpu - last_cpu_ns;
            st.process_cpu_per_core_tenths = (int32_t)(delta_cpu * 1000 / (n - last_tick_ns));
        }
        last_tick_ns = n;
        last_cpu_ns = cpu;
        st.sensor_age_ms = (uint32_t)((n - last_sensor_ns) / 1000000);
        st.monotonic_ns = n;
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}
#endif
static void sig_stop(int sig)
{
    (void)sig;
    running = 0;
}

static void snapshot(pg_status_t *dst)
{
    pthread_mutex_lock(&mu);
    *dst = st;
    uint64_t n = now_ns(CLOCK_MONOTONIC);
    dst->monotonic_ns = n;
    dst->sensor_age_ms = (uint32_t)((n - last_sensor_ns) / 1000000);
    pthread_mutex_unlock(&mu);
}

static void write_csv(FILE *fp, const pg_status_t *s)
{
    fprintf(fp,
            "%" PRIu64 ",%d,%d,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 "\n",
            s->monotonic_ns, s->scenario, s->temperature_milli_c, s->sensor_age_ms,
            s->process_cpu_per_core_tenths, s->level_requested, s->level_applied,
            s->supervisor_state, s->critical_jobs, s->critical_misses, s->critical_max_response_us,
            s->critical_max_jitter_us, s->last_actuation_us, s->recovery_to_full_ms);
    fflush(fp);
}

static void *csv_writer(void *ptr)
{
    FILE *fp = ptr;
    while (running) {
        sleep_until(now_ns(CLOCK_MONOTONIC) + NS);
        pg_status_t s;
        snapshot(&s);
        write_csv(fp, &s);
    }
    return NULL;
}

static int apply_caps(const pg_cap_request_t *cmd, uint64_t now)
{
    int nc1 = cmd->request.arg;
    int nc2 = cmd->request.extra;
    if (!pg_progress_allows(progress, (unsigned)nc1, (unsigned)nc2)) {
        return ESTALE;
    }
    if (nc1 < 0 || nc1 > 100 || nc2 < 0 || nc2 > 100) {
        return EINVAL;
    }
    if (cmd->request.command == PG_CMD_LEASED_CAPS) {
        if (!cmd->epoch || !cmd->decision_id || cmd->until_ns <= now || cmd->epoch < cap_epoch ||
            (cmd->epoch == cap_epoch && cmd->decision_id < cap_id)) {
            return ESTALE;
        }
        if (cmd->epoch == cap_epoch && cmd->decision_id == cap_id) {
            int target1 = stop_nc1_after_nc2 ? 0 : nc1_cap;
            if (lease_until_ns > now && nc1 == target1 && nc2 == nc2_cap) {
                if (cmd->until_ns > lease_until_ns) {
                    lease_until_ns = cmd->until_ns;
                }
                return 0;
            }

            if (nc1 || nc2) {
                return ESTALE;
            }
        }
        cap_epoch = cmd->epoch;
        cap_id = cmd->decision_id;
        lease_until_ns = cmd->until_ns;
    } else if (cap_epoch) {
        return EBUSY;
    }

    if (!nc1_cap && !nc2_cap && (nc1 || nc2) && !recover_ns) {
        recover_ns = now;
    }
    stop_nc1_after_nc2 = nc1_cap && nc2_cap && !nc1 && !nc2;
    if (!stop_nc1_after_nc2) {
        nc1_cap = nc1;
    }
    nc2_cap = nc2;
    level = !nc1 ? 2 : !nc2 ? 1 : 0;
    st.level_requested = level;
    level_command_ns = now;
    ack_ns[0] = ack_ns[1] = 0;
    pthread_cond_broadcast(&cap_changed);
    return 0;
}

static void serve(const char *csv_path)
{
    int cap_error = init_cap_condition();
    if (cap_error) {
        fprintf(stderr, "cap condition: %s\n", strerror(cap_error));
        exit(1);
    }
    name_attach_t *attach = name_attach(NULL, PG_BENCH_NAME, 0);
    if (!attach) {
        perror("name_attach");
        exit(1);
    }
    progress = pg_progress_open(1);
    if (!progress) {
        perror("bench progress mapping");
        exit(1);
    }
    FILE *fp = fopen(csv_path, "w");
    if (!fp) {
        perror(csv_path);
        name_detach(attach, 0);
        exit(1);
    }
    fprintf(
        fp,
        "monotonic_ns,scenario,temp_sim_milli_c,sensor_age_ms,process_cpu_per_core_tenths,level_requested,level_applied,supervisor_state,critical_jobs,deadline_misses,max_response_us,max_release_jitter_us,last_actuation_us,recovery_to_full_ms\n");
    start_ns = last_sensor_ns = scenario_started_ns = now_ns(CLOCK_MONOTONIC);
    st.temperature_milli_c = 45000;
    last_cpu_ns = now_ns(CLOCK_PROCESS_CPUTIME_ID);
    last_tick_ns = start_ns;
    st.supervisor_state = PG_ST_UNKNOWN;
    st.level_requested = st.level_applied = level;
    st.nc1_cap_pct = nc1_cap;
    st.nc2_cap_pct = nc2_cap;
    pthread_t critical;
    pthread_t nc1;
    pthread_t nc2;
    pthread_t sample;
    pthread_t logger;
    if (pthread_create(&critical, NULL, critical_worker, NULL) ||
        pthread_create(&nc1, NULL, noncritical_worker, (void *)(intptr_t)0) ||
        pthread_create(&nc2, NULL, noncritical_worker, (void *)(intptr_t)1) ||
        pthread_create(&sample, NULL, sampler, NULL) ||
        pthread_create(&logger, NULL, csv_writer, fp)) {
        fprintf(stderr, "thread creation failed\n");
        exit(1);
    }
    printf("pgbench PID %ld: QNX name=%s; CSV=%s (simulated temperature)\n", (long)getpid(),
           PG_BENCH_NAME, csv_path);
    fflush(stdout);
    while (running) {
        union {
            pg_request_t req;
            pg_cap_request_t caps;
            struct _pulse pulse;
        } msg;

        memset(&msg, 0, sizeof msg);
        struct _msg_info info;
        int rcvid = MsgReceive(attach->chid, &msg, sizeof msg, &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("MsgReceive");
            break;
        }
        if (rcvid == 0) {
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(msg.pulse.scoid);
            }
            if (msg.pulse.code == PG_PULSE_FORCE_OFF) {
                pthread_cond_broadcast(&cap_changed);
            }
            if (msg.pulse.code == _PULSE_CODE_UNBLOCK) {
                struct _msg_info pending;
                if (MsgInfo(msg.pulse.value.sival_int, &pending) != -1 &&
                    (pending.flags & _NTO_MI_UNBLOCK_REQ)) {
                    MsgError(msg.pulse.value.sival_int, -1);
                }
            }
            continue;
        }
        if (msg.req.type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        pg_status_t result;
        int err = 0;
        pthread_mutex_lock(&mu);
        size_t expected =
            msg.req.command == PG_CMD_LEASED_CAPS ? sizeof(msg.caps) : sizeof(msg.req);
        if (msg.req.type != PG_BENCH_TYPE || msg.req.version != PG_BENCH_VERSION ||
            info.srcmsglen != (ssize_t)expected || info.msglen != (ssize_t)expected) {
            err = EPROTO;
        } else {
            switch (msg.req.command) {
                case PG_CMD_STATUS:
                    break;
                case PG_CMD_SCENARIO:
                    if (msg.req.arg < PG_SC_NORMAL || msg.req.arg > PG_SC_FREEZE) {
                        err = EINVAL;
                    } else {
                        scenario = msg.req.arg;
                        st.scenario = scenario;
#if defined(PG_BENCH_TRIAL)
                        worker_fault = scenario == PG_SC_FREEZE ? msg.req.extra : 0;
                        scenario_started_ns = now_ns(CLOCK_MONOTONIC);
                        if (scenario == PG_SC_RAMP && msg.req.extra == 1) {
                            ftti_start_ns = scenario_started_ns;
                            ftti_first_ns = ftti_80_ns = 0;
                        } else {
                            ftti_start_ns = 0;
                            last_sensor_ns = scenario_started_ns;
                        }
#else
                        scenario_started_ns = last_sensor_ns = now_ns(CLOCK_MONOTONIC);
#endif
                        st.sensor_age_ms = 0;
                        st.temperature_milli_c =
                            scenario == PG_SC_NORMAL ? 45000 : st.temperature_milli_c;
                    }
                    break;
                case PG_CMD_LEVEL:
                    if (pg_progress_forced(progress) && msg.req.arg != 2) {
                        err = ESTALE;
                        break;
                    }
                    if (cap_epoch) {
                        err = EBUSY;
                        break;
                    }
                    if (msg.req.arg < 0 || msg.req.arg > 2) {
                        err = EINVAL;
                    } else {
                        int old = level;
                        stop_nc1_after_nc2 = 0;
                        level = msg.req.arg;
                        st.level_requested = level;
                        nc1_cap = level < 2 ? 100 : 0;
                        nc2_cap = level < 1 ? 100 : 0;
                        level_command_ns = now_ns(CLOCK_MONOTONIC);
                        ack_ns[0] = ack_ns[1] = 0;
                        pthread_cond_broadcast(&cap_changed);
                        if (old == 2 && level < 2 && recover_ns == 0) {
                            recover_ns = level_command_ns;
                        }
                    }
                    break;
                case PG_CMD_LEASED_CAPS:
                case PG_CMD_CAPS:
                    err = apply_caps(&msg.caps, now_ns(CLOCK_MONOTONIC));
                    break;
                case PG_CMD_STATE:
                    if (msg.req.arg < PG_ST_UNKNOWN || msg.req.arg > PG_ST_RECOVERY ||
                        msg.req.extra < 0 || msg.req.extra > 100) {
                        err = EINVAL;
                    } else {
                        current_state = pg_progress_forced(progress) ? PG_ST_CRITICAL : msg.req.arg;
                        st.supervisor_state = current_state;
                        st.recovery_percent = pg_progress_forced(progress) ? 0 : msg.req.extra;
                    }
                    break;
                case PG_CMD_STOP:
                    running = 0;
                    pthread_cond_broadcast(&cap_changed);
                    break;
                default:
                    err = EINVAL;
                    break;
            }
        }
        result = st;
        result.thermal_acquired_ns = last_sensor_ns;
        result.cpu_window_end_ns = last_tick_ns;
        result.monotonic_ns = now_ns(CLOCK_MONOTONIC);
        result.sensor_age_ms = (uint32_t)((result.monotonic_ns - last_sensor_ns) / 1000000);
        result.error = err;
        pthread_mutex_unlock(&mu);
        MsgReply(rcvid, EOK, &result, sizeof result);
    }
    name_detach(attach, 0);

    pthread_join(critical, NULL);
    pthread_join(nc1, NULL);
    pthread_join(nc2, NULL);
    pthread_join(sample, NULL);
    pthread_join(logger, NULL);
#if defined(PG_BENCH_TRIAL)
    export_jobs();
    export_effects();
#endif
    pthread_cond_destroy(&cap_changed);
    fclose(fp);
    pg_progress_close(progress, 1);
}

static void print_status(const pg_status_t *s)
{
    const char *names[] = {"normal", "ramp", "spike", "freeze"};
    const char *states[] = {"UNKNOWN", "NORMAL", "WARNING", "THROTTLED", "CRITICAL", "RECOVERY"};
    printf("Scenario: %s | temperature: %.1f C (SIMULATED), age: %u ms\n", names[s->scenario],
           s->temperature_milli_c / 1000., s->sensor_age_ms);
    printf("CPU: %.1f%% of one core (process CPU time) | NC1 cap: %d%% | NC2 cap: %d%%\n",
           s->process_cpu_per_core_tenths / 10., s->nc1_cap_pct, s->nc2_cap_pct);
    printf("Restriction: requested=%d applied=%d | supervisor=%s | recovery=%d%%\n",
           s->level_requested, s->level_applied, states[s->supervisor_state], s->recovery_percent);
    printf("Critical: jobs=%" PRIu64 " misses=%" PRIu64 " max_response=%" PRIu64
           " us max_jitter=%" PRIu64 " us FIFO=%s\n",
           s->critical_jobs, s->critical_misses, s->critical_max_response_us,
           s->critical_max_jitter_us, s->critical_fifo_ok ? "YES" : "NO (fallback)");
    printf("Last requested-level-to-worker-ACK=%" PRIu64 " us | recovery-to-full=%" PRIu64 " ms\n",
           s->last_actuation_us, s->recovery_to_full_ms);
}

static void usage(const char *argv0)
{
    fprintf(
        stderr,
        "Usage:\n  %s serve [csv_file]\n  %s status\n  %s dashboard\n  %s scenario normal|ramp|spike|freeze\n  %s level 0|1|2\n  %s state unknown|normal|warning|throttled|critical|recovery [progress_0_to_100]\n  %s stop\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static int dashboard_send(int coid, unsigned command, int arg, pg_status_t *reply)
{
    pg_request_t req = {PG_BENCH_TYPE, PG_BENCH_VERSION, command, arg, 0};
    if (MsgSend(coid, &req, sizeof(req), reply, sizeof(*reply)) == -1) {
        perror("MsgSend");
        return 0;
    }
    if (reply->error) {
        fprintf(stderr, "pgbench error: %s\n", strerror(reply->error));
        return 0;
    }
    return 1;
}

static void dashboard(int coid)
{
    static const char *names[] = {"normal", "ramp", "spike", "freeze"};
    struct termios saved;
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &saved) == -1) {
        perror("tcgetattr (use a terminal)");
        return;
    }
    raw = saved;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        perror("tcsetattr");
        return;
    }
    int selected = -1;
    for (;;) {
        pg_status_t reply;
        if (!dashboard_send(coid, PG_CMD_STATUS, 0, &reply)) {
            break;
        }
        if (selected < 0) {
            selected = reply.scenario;
        }
        printf(
            "\033[2J\033[HPOWERGUARD LIVE DASHBOARD\nUp/Down: change active scenario | q: quit\n\n");
        print_status(&reply);
        printf("\nActive scenario: %s\n", names[reply.scenario]);
        fflush(stdout);
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(STDIN_FILENO, &ready);
        struct timeval refresh = {.tv_sec = 0, .tv_usec = 500000};
        int available = select(STDIN_FILENO + 1, &ready, NULL, NULL, &refresh);
        if (available < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        if (available == 0) {
            continue;
        }
        int c = getchar();
        if (c == 'q' || c == 'Q') {
            break;
        }
        int next = selected;
        if (c == 27 && getchar() == '[') {
            c = getchar();
            if (c == 'A' && selected < PG_SC_FREEZE) {
                ++next;
            } else if (c == 'B' && selected > PG_SC_NORMAL) {
                --next;
            }
        }
        if (next != selected) {
            selected = next;
            (void)dashboard_send(coid, PG_CMD_SCENARIO, selected, &reply);
        }
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    puts("\033[2J\033[H");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "serve")) {
        signal(SIGINT, sig_stop);
        signal(SIGTERM, sig_stop);
        serve(argc >= 3 ? argv[2] : "pgbench.csv");
        return 0;
    }
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "dashboard")) {
        int coid = name_open(PG_BENCH_NAME, 0);
        if (coid == -1) {
            perror("name_open (is pgbench serve running?)");
            return 1;
        }
        dashboard(coid);
        name_close(coid);
        return 0;
    }
    pg_request_t req = {PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_STATUS, 0, 0};
    if (!strcmp(argv[1], "status")) {
    } else if (!strcmp(argv[1], "stop")) {
        req.command = PG_CMD_STOP;
    } else if (!strcmp(argv[1], "level") && argc >= 3) {
        req.command = PG_CMD_LEVEL;
        req.arg = atoi(argv[2]);
    } else if (!strcmp(argv[1], "scenario") && argc >= 3) {
        const char *options[] = {"normal", "ramp", "spike", "freeze"};
        int i;
        for (i = 0; i < 4; ++i) {
            if (!strcmp(argv[2], options[i])) {
                break;
            }
        }
        if (i == 4) {
            usage(argv[0]);
            return 2;
        }
        req.command = PG_CMD_SCENARIO;
        req.arg = i;
    } else if (!strcmp(argv[1], "state") && argc >= 3) {
        const char *options[] = {"unknown",   "normal",   "warning",
                                 "throttled", "critical", "recovery"};
        int i;
        for (i = 0; i < 6; ++i) {
            if (!strcmp(argv[2], options[i])) {
                break;
            }
        }
        if (i == 6) {
            usage(argv[0]);
            return 2;
        }
        req.command = PG_CMD_STATE;
        req.arg = i;
        if (argc >= 4) {
            req.extra = atoi(argv[3]);
        }
    } else {
        usage(argv[0]);
        return 2;
    }
    int coid = name_open(PG_BENCH_NAME, 0);
    if (coid == -1) {
        perror("name_open (is pgbench serve running?)");
        return 1;
    }
    pg_status_t reply;
    memset(&reply, 0, sizeof reply);
    int rc = MsgSend(coid, &req, sizeof req, &reply, sizeof reply);
    name_close(coid);
    if (rc == -1) {
        perror("MsgSend");
        return 1;
    }
    if (reply.error) {
        fprintf(stderr, "pgbench error: %s\n", strerror(reply.error));
        return 1;
    }
    print_status(&reply);
    return 0;
}
