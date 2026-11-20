/* The supervisor owns the policy state. Clients on this board share its IPC layout. */
#include "ipc.h"
#include "pg_threadctl.h"
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
    if (clock_gettime(CLOCK_MONOTONIC, &t) == -1) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* Trace transitions between polls so a short-lived restriction is visible on the trace. */
static void trace_change(const pg_status *before, const pg_status *after) {
    if (before->decision_id != after->decision_id ||
        before->state != after->state ||
        before->causes != after->causes ||
        before->effective_ns != after->effective_ns ||
        before->clear_since_ns != after->clear_since_ns) {
        (void)trace_logb(80, after, sizeof(*after));
    }
}

int main(int argc, char **argv) {
    char *end;
    if (argc != 2) {
        fprintf(stderr, "usage: PowerGuard FIFO_PRIORITY\n");
        return 2;
    }

    long priority = strtol(argv[1], &end, 10);
    if (*end || priority < sched_get_priority_min(SCHED_FIFO) ||
        priority > sched_get_priority_max(SCHED_FIFO)) {
        return 2;
    }

    struct sched_param sched = {.sched_priority = (int)priority};
    if (sched_setscheduler(0, SCHED_FIFO, &sched) == -1) {
        perror("SCHED_FIFO");
        return 1;
    }

    pg_thread_report placement;
    if (priority >= 30 || pg_thread_configure(0, (int)priority, "pg-supervisor", &placement)) {
        fputs("Supervisor requires verified CPU0 affinity and priority below critical priority 30\n", stderr);
        return 1;
    }
    pg_thread_print(&placement);

    name_attach_t *server = name_attach(NULL, PG_NAME, 0);
    if (!server) {
        perror("name_attach");
        return 1;
    }

    /* Keep checking sample age even when clients stop sending messages. */
    int coid = ConnectAttach(0, 0, server->chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("ConnectAttach");
        return 1;
    }

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
        union {
            struct _pulse pulse;
            pg_message_v1 message;
            pg_sample_batch batch;
        } in;
        struct _msg_info info;
        rcvid_t rcvid = MsgReceive(server->chid, &in, sizeof(in), &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("MsgReceive");
            return 1;
        }
        uint64_t now = monotonic_ns();
        pg_status before = policy.status;
        if (rcvid == 0) {
            if (in.pulse.code == _PULSE_CODE_MINAVAIL) {
                armed_ns = 0;
                pg_tick(&policy, now);
            }
            pg_observe(&metrics, &before, &policy);
            trace_change(&before, &policy.status);
            if (in.pulse.code == _PULSE_CODE_DISCONNECT)
                ConnectDetach(in.pulse.scoid);
            continue;
        }
        const pg_message_v1 *msg = &in.message;
        if (info.msglen >= (ssize_t)sizeof(msg->type) && msg->type == PG_SAMPLE_BATCH) {
            pg_sample_batch *b = &in.batch;
            if (info.srcmsglen != sizeof(*b) || info.msglen != sizeof(*b) ||
                b->version != PG_VERSION || !b->count || b->count > PG_BATCH_MAX) {
                MsgError(rcvid, EINVAL);
                continue;
            }
            for (unsigned i = 0; i < b->count; ++i) {
                before = policy.status;
                pg_ingest(&policy, &b->samples[i], now);
                pg_observe(&metrics, &before, &policy);
                trace_change(&before, &policy.status);
            }
            MsgReply(rcvid, EOK, &now, sizeof(now));
            continue;
        }
        if (info.msglen >= (ssize_t)sizeof(msg->type) && msg->type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (info.srcmsglen != sizeof(*msg) || info.msglen != sizeof(*msg) ||
            msg->version != PG_VERSION) {
            MsgError(rcvid, EINVAL);
            continue;
        }

        switch (msg->type) {
        case PG_SAMPLE:
            pg_ingest(&policy, &msg->body.sample, now);
            break;

        case PG_SAMPLE_POWER:
            pg_ingest_power(&policy, &msg->body.sample,
                            msg->reserved <= 100 ? (int)msg->reserved : -1, now);
            break;

        case PG_JOB:
            if (msg->body.job.epoch != policy.status.epoch ||
                !pg_job(&metrics, msg->body.job.id, msg->body.job.release_ns,
                        msg->body.job.deadline_ns, msg->body.job.finish_ns, now)) {
                MsgError(rcvid, EINVAL);
                continue;
            }
            pg_tick(&policy, now);
            break;

        case PG_TRIAL:
            if (msg->body.trial.epoch != policy.status.epoch ||
                policy.status.state != msg->body.trial.from ||
                !pg_trial(&metrics, msg->body.trial.series, msg->body.trial.from,
                          msg->body.trial.to, msg->body.trial.trigger_ns, now)) {
                MsgError(rcvid, EINVAL);
                continue;
            }
            pg_tick(&policy, now);
            break;

        case PG_SCORE:
            if (!msg->body.trial.series || msg->body.trial.series > PG_CASES ||
                msg->body.trial.from > RECOVERY || msg->body.trial.to > RECOVERY) {
                MsgError(rcvid, EINVAL);
                continue;
            }
            pg_tick(&policy, now);
            break;

        case PG_MANAGER_HEARTBEAT:
            if (!pg_manager_progress(&policy, msg->body.manager.epoch, msg->body.manager.sequence,
                                     msg->body.manager.progress_ns, now)) {
                MsgError(rcvid, EINVAL);
                continue;
            }
            break;

        case PG_EFFECT:
        case PG_EFFECT_CONFIRMED:
            /* A cap acknowledgement can arrive after its lease has expired. */
            pg_tick(&policy, now);
            if (!pg_effect(&policy, msg->body.effect.epoch, msg->body.effect.decision_id,
                           msg->body.effect.effective_ns, msg->body.effect.nc1_pct,
                           msg->body.effect.nc2_pct, now)) {
                pg_observe(&metrics, &before, &policy);
                trace_change(&before, &policy.status);
                MsgError(rcvid, EINVAL);
                continue;
            }
            break;

        case PG_READ:
        case PG_CONFIRM_READ:
            pg_tick(&policy, now);
            break;

        default:
            MsgError(rcvid, ENOSYS);
            continue;
        }
        pg_observe(&metrics, &before, &policy);
        trace_change(&before, &policy.status);
        pg_reply_v1 reply = {
            .version = PG_VERSION,
            .config_generation = 1,
            .decision = policy.status,
            .last_input = policy.sample
        };
        if (msg->type == PG_EFFECT_CONFIRMED || msg->type == PG_CONFIRM_READ) {
            pg_confirmation_reply receipt = {
                .snapshot = reply,
                .detected_ns = policy.restriction_decision_id == policy.status.decision_id ?
                               policy.restriction_from_ns : policy.status.issued_ns,
                .received_ns = policy.effect_received_id == policy.status.decision_id ? policy.effect_received_ns : 0,
                .manager_progress_ns = policy.manager_progress_ns,
                .manager_progress_seq = policy.manager_progress_seq,
                .restriction = policy.restriction_decision_id == policy.status.decision_id
            };
            MsgReply(rcvid, EOK, &receipt, sizeof(receipt));
        } else if (msg->type == PG_SCORE) {
            unsigned series = msg->body.trial.series;
            unsigned from = msg->body.trial.from;
            unsigned to = msg->body.trial.to;
            pg_score_reply report = {
                .snapshot = reply,
                .scores = metrics.scores,
                .jitter = metrics.trials[series - 1][from][to],
                .series = series,
                .from = from,
                .to = to
            };
            MsgReply(rcvid, EOK, &report, sizeof(report));
        } else {
            MsgReply(rcvid, EOK, &reply, sizeof(reply));
        }
    }
}
