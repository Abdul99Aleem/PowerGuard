/* K's single-owner policy service. Only local same-ABI clients use this draft. */
#include "ipc.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/trace.h>
#include <time.h>

#define PULSE_TICK _PULSE_CODE_MINAVAIL        /* periodic policy tick */
#define PULSE_AGE (_PULSE_CODE_MINAVAIL + 1)  /* temperature age deadline */

static uint64_t monotonic_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) == -1) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* A timer that delivers the given pulse code to our own channel. The
   tolerance call comes first so the timer gets high resolution. */
static int pulse_timer(int coid, int priority, int code, timer_t *timer) {
    struct sigevent event;
    struct itimerspec tolerance = {.it_value = {.tv_nsec = 1}};
    SIGEV_PULSE_INIT(&event, coid, priority, code, 0);
    if (timer_create(CLOCK_MONOTONIC, &event, timer) == -1) return -1;
    return timer_settime(*timer, TIMER_TOLERANCE, &tolerance, NULL);
}

/* One-shot at an absolute CLOCK_MONOTONIC time. It fires even if the sampler
   has stopped sending, which is the point of an independent expiry. */
static int arm_at(timer_t timer, uint64_t deadline) {
    struct itimerspec at = {.it_value = {.tv_sec = (time_t)(deadline / UINT64_C(1000000000)),
                                         .tv_nsec = (long)(deadline % UINT64_C(1000000000))}};
    return timer_settime(timer, TIMER_ABSTIME, &at, NULL);
}

/* Enable user event 80 in the QNX trace session to retain state, reasons,
   dwell starts, commands and matched effects, even between CLI refreshes. */
static void trace_change(const pg_status *old, const pg_status *s) {
    if (old->decision_id != s->decision_id || old->state != s->state ||
        old->causes != s->causes || old->effective_ns != s->effective_ns ||
        old->clear_since_ns != s->clear_since_ns)
        (void)trace_logb(80, s, sizeof(*s));
}

int main(int argc, char **argv) {
    char *end;
    if (argc != 2) { fprintf(stderr, "usage: PowerGuard FIFO_PRIORITY\n"); return 2; }
    long priority = strtol(argv[1], &end, 10);
    if (*end || priority < sched_get_priority_min(SCHED_FIFO) ||
        priority > sched_get_priority_max(SCHED_FIFO)) return 2;
    struct sched_param sched = {.sched_priority = (int)priority};
    if (sched_setscheduler(0, SCHED_FIFO, &sched) == -1) { perror("SCHED_FIFO"); return 1; }
    name_attach_t *server = name_attach(NULL, PG_NAME, 0);
    if (!server) { perror("name_attach"); return 1; }
    int coid = ConnectAttach(0, 0, server->chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) { perror("ConnectAttach"); return 1; }
    timer_t tick_timer, age_timer;
    struct itimerspec tick = {.it_interval = {.tv_nsec = PG_TICK},
                              .it_value = {.tv_nsec = PG_TICK}};
    if (pulse_timer(coid, (int)priority, PULSE_TICK, &tick_timer) == -1 ||
        timer_settime(tick_timer, 0, &tick, NULL) == -1 ||
        pulse_timer(coid, (int)priority, PULSE_AGE, &age_timer) == -1) {
        perror("policy timers"); return 1;
    }
    pg_metrics metrics = {0};
    pg_policy policy;
    pg_init(&policy, monotonic_ns());
    uint64_t armed_deadline = 0;
    for (;;) {
        uint64_t deadline = pg_age_deadline(&policy);
        if (deadline && deadline != armed_deadline) {
            if (arm_at(age_timer, deadline) == -1) { perror("age timer"); return 1; }
            armed_deadline = deadline;
        }
        union { struct _pulse pulse; pg_message_v1 message; } in;
        struct _msg_info info;
        rcvid_t id = MsgReceive(server->chid, &in, sizeof(in), &info);
        if (id == -1) { if (errno == EINTR) continue; perror("MsgReceive"); return 1; }
        uint64_t now = monotonic_ns();
        pg_status before = policy.status;
        if (id == 0) {
            if (in.pulse.code == PULSE_TICK || in.pulse.code == PULSE_AGE) pg_tick(&policy, now);
            pg_observe(&metrics, &before, &policy);
            trace_change(&before, &policy.status);
            if (in.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(in.pulse.scoid);
            continue;
        }
        pg_message_v1 *m = &in.message;
        if (info.msglen >= (ssize_t)sizeof(m->type) && m->type == _IO_CONNECT) {
            MsgReply(id, EOK, NULL, 0); continue;
        }
        if (info.srcmsglen != sizeof(*m) || info.msglen != sizeof(*m) ||
            m->version != PG_VERSION) { MsgError(id, EINVAL); continue; }
        switch (m->type) {
        case PG_SAMPLE:
        case PG_SAMPLE_POWER: /* old senders; P is now derived from the demands */
            pg_ingest(&policy, &m->body.sample, now);
            break;
        case PG_JOB:
            if (m->body.job.epoch != policy.status.epoch ||
                !pg_job(&metrics, m->body.job.id, m->body.job.release_ns,
                        m->body.job.deadline_ns, m->body.job.finish_ns, now)) {
                MsgError(id, EINVAL); continue;
            }
            pg_tick(&policy, now);
            break;
        case PG_TRIAL:
            if (m->body.trial.epoch != policy.status.epoch ||
                policy.status.state != m->body.trial.from ||
                !pg_trial(&metrics, m->body.trial.series, m->body.trial.from,
                          m->body.trial.to, m->body.trial.trigger_ns, now)) {
                MsgError(id, EINVAL); continue;
            }
            pg_tick(&policy, now);
            break;
        case PG_SCORE:
            if (!m->body.trial.series || m->body.trial.series > PG_CASES ||
                m->body.trial.from > RECOVERY || m->body.trial.to > RECOVERY) {
                MsgError(id, EINVAL); continue;
            }
            pg_tick(&policy, now);
            break;
        case PG_EFFECT:
            pg_tick(&policy, now);
            if (!pg_effect(&policy, m->body.effect.epoch, m->body.effect.decision_id,
                           m->body.effect.effective_ns, m->body.effect.nc1_pct,
                           m->body.effect.nc2_pct, now)) {
                pg_observe(&metrics, &before, &policy);
                trace_change(&before, &policy.status);
                MsgError(id, EINVAL); continue;
            }
            break;
        case PG_WORKER_EFFECT:
            pg_tick(&policy, now);
            if (!pg_worker_effect(&policy, m->body.worker_effect.epoch,
                                  m->body.worker_effect.decision_id, m->body.worker_effect.nc,
                                  m->body.worker_effect.cap_pct,
                                  m->body.worker_effect.applied_ns, now)) {
                pg_observe(&metrics, &before, &policy);
                trace_change(&before, &policy.status);
                MsgError(id, EINVAL); continue;
            }
            break;
        case PG_READ:
            pg_tick(&policy, now);
            break;
        default:
            MsgError(id, ENOSYS); continue;
        }
        pg_observe(&metrics, &before, &policy);
        trace_change(&before, &policy.status);
        pg_reply_v1 reply = {.version = PG_VERSION, .config_generation = 1,
                             .decision = policy.status, .last_input = policy.sample};
        if (m->type == PG_SCORE) {
            unsigned series = m->body.trial.series, from = m->body.trial.from, to = m->body.trial.to;
            pg_score_reply report = {.snapshot = reply, .scores = metrics.scores,
                .jitter = metrics.trials[series-1][from][to], .series = series, .from = from, .to = to};
            MsgReply(id, EOK, &report, sizeof(report));
        } else MsgReply(id, EOK, &reply, sizeof(reply));
    }
}
