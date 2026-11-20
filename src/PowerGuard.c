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

static uint64_t monotonic_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) == -1) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
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
    struct sigevent event;
    SIGEV_PULSE_INIT(&event, coid, (int)priority, _PULSE_CODE_MINAVAIL, 0);
    timer_t timer;
    struct itimerspec tolerance = {.it_value = {.tv_nsec = 1}};
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) == -1 ||
        timer_settime(timer, TIMER_TOLERANCE, &tolerance, NULL) == -1) {
        perror("policy timer");
        return 1;
    }

    pg_metrics metrics = {0};
    pg_policy policy;
    pg_init(&policy, monotonic_ns());
    uint64_t armed_ns = 0;
    for (;;) {
        uint64_t next = pg_next_deadline(&policy);
        if (next != armed_ns) {
            struct itimerspec expiry = {
                .it_value = {.tv_sec = next / UINT64_C(1000000000),
                             .tv_nsec = next % UINT64_C(1000000000)}
            };
            if (timer_settime(timer, TIMER_ABSTIME, &expiry, NULL) == -1) {
                perror("arm policy deadline");
                return 1;
            }
            armed_ns = next;
        }
        union { struct _pulse pulse; pg_message_v1 message; pg_sample_batch batch; } in;
        struct _msg_info info;
        rcvid_t id = MsgReceive(server->chid, &in, sizeof(in), &info);
        if (id == -1) { if (errno == EINTR) continue; perror("MsgReceive"); return 1; }
        uint64_t now = monotonic_ns();
        pg_status before = policy.status;
        if (id == 0) {
            if (in.pulse.code == _PULSE_CODE_MINAVAIL) {
                armed_ns = 0;
                pg_tick(&policy, now);
            }
            pg_observe(&metrics, &before, &policy);
            trace_change(&before, &policy.status);
            if (in.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(in.pulse.scoid);
            continue;
        }
        pg_message_v1 *m = &in.message;
        if (info.msglen >= (ssize_t)sizeof(m->type) && m->type == PG_SAMPLE_BATCH) {
            pg_sample_batch *b = &in.batch;
            if (info.srcmsglen != sizeof(*b) || info.msglen != sizeof(*b) ||
                b->version != PG_VERSION || !b->count || b->count > PG_BATCH_MAX) {
                MsgError(id, EINVAL);
                continue;
            }
            for (unsigned i = 0; i < b->count; ++i) {
                before = policy.status;
                pg_ingest(&policy, &b->samples[i], now);
                pg_observe(&metrics, &before, &policy);
                trace_change(&before, &policy.status);
            }
            MsgReply(id, EOK, &now, sizeof(now));
            continue;
        }
        if (info.msglen >= (ssize_t)sizeof(m->type) && m->type == _IO_CONNECT) {
            MsgReply(id, EOK, NULL, 0); continue;
        }
        if (info.srcmsglen != sizeof(*m) || info.msglen != sizeof(*m) ||
            m->version != PG_VERSION) { MsgError(id, EINVAL); continue; }
        switch (m->type) {
        case PG_SAMPLE:
            pg_ingest(&policy, &m->body.sample, now);
            break;
        case PG_SAMPLE_POWER:
            pg_ingest_power(&policy, &m->body.sample,
                            m->reserved <= 100 ? (int)m->reserved : -1, now);
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
