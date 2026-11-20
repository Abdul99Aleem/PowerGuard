#include "protocol.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static uint64_t ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) _exit(1);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void sleep_to(uint64_t when) {
    struct timespec t = {when / 1000000000, when % 1000000000};
    int e;
    do {
        uint64_t tolerance = 1;
        if (TimerTimeout(CLOCK_MONOTONIC, TIMER_TOLERANCE, NULL, &tolerance, NULL) == -1) {
            perror("worker high resolution tolerance"); _exit(1);
        }
        e = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    } while (running && e == EINTR);
    if (e && e != EINTR) { errno = e; perror("worker absolute release"); _exit(1); }
}
static void compute(unsigned iterations) {
    volatile unsigned x = 1;
    for (unsigned i = 0; i < iterations; ++i) x = x * 1664525u + 1013904223u;
}
int pg_worker_main(unsigned role, unsigned iterations) {
    if (role > 2 || !iterations) return 2;
    const char *names[] = {"pg-critical", "pg-nc1", "pg-nc2"};
    const int priorities[] = {30, 10, 8};
    pg_worker_message message = {.type = PG_RUNTIME_TYPE, .version = PG_RUNTIME_VERSION, .role = role};
    if (pg_thread_configure(role + 1, priorities[role], names[role], &message.placement)) {
        perror("worker ThreadCtl/scheduling qualification"); return 1;
    }
    pg_thread_print(&message.placement);
    signal(SIGINT, stop); signal(SIGTERM, stop);
    int coid = name_open(PG_BENCH_NAME, 0);
    if (coid == -1) { perror("worker name_open"); return 1; }
    uint64_t period = role ? PG_WORKER_PERIOD : PG_CRITICAL_PERIOD;
    uint64_t next = ns(), id = 0, last_epoch = 0, last_id = 0, effective = 0, until = 0;
    int cap = 0, fault = 0;
    while (running) {
        uint64_t now = ns();
        if (role && cap && (!until || now >= until)) { cap = 0; effective = now; }
        message.applied_cap = cap; message.epoch = last_epoch; message.decision_id = last_id;
        message.effective_ns = effective; message.seen_ns = now;
        if (pg_thread_verify(&message.placement) ||
            message.placement.runmask != (UINT64_C(1) << (role + 1)) ||
            message.placement.inherit_mask != (UINT64_C(1) << (role + 1)) ||
            message.placement.cpu != (int)(role + 1) ||
            message.placement.policy != SCHED_FIFO || message.placement.priority != priorities[role]) {
            fault = 1; cap = 0;
        }
        message.fault = fault;
        pg_worker_reply reply = {0};
        uint64_t timeout = role ? UINT64_C(5000000) : UINT64_C(2000000);
        int ok = TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                              NULL, &timeout, NULL) != -1 &&
                 MsgSend(coid, &message, sizeof(message), &reply, sizeof(reply)) != -1 && !reply.error;
        if (!ok) { ++message.dropped; cap = 0; }
        message.job = (pg_job_record){0};
        if (ok && reply.stop) break;
        if (fault) { fputs("Worker placement/scheduling drift: closed\n", stderr); break; }
        now = ns();
        if (role && ok) {
            int valid = reply.epoch && reply.decision_id && reply.until_ns > now &&
                reply.epoch >= last_epoch && (reply.epoch > last_epoch || reply.decision_id >= last_id);
            int same = reply.epoch == last_epoch && reply.decision_id == last_id;
            int acknowledge = 0;
            /* An expired generation cannot reopen a noncritical gate. */
            if (valid && same && until && now >= until && reply.cap) valid = 0;
            if (valid && (!same || cap != reply.cap)) {
                effective = now; cap = reply.cap;
                acknowledge = 1;
            }
            if (valid) { last_epoch = reply.epoch; last_id = reply.decision_id; until = reply.until_ns; }
            else cap = 0;
            message.applied_cap = cap;
            /* This boundary has completed the previous chunk. Report the
             * newly installed gate immediately, rather than adding another
             * worker period to the ordered two-worker acknowledgement chain.
             */
            if (acknowledge) {
                message.epoch = last_epoch; message.decision_id = last_id;
                message.effective_ns = effective; message.seen_ns = ns();
                pg_worker_reply ack = {0};
                timeout = UINT64_C(5000000);
                if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                                 NULL, &timeout, NULL) == -1 ||
                    MsgSend(coid, &message, sizeof(message), &ack, sizeof(ack)) == -1 || ack.error) {
                    ++message.dropped; cap = 0;
                }
                if (ack.stop) break;
            }
            if (cap > 0) {
                uint64_t end = now + (uint64_t)reply.demand_pct * (unsigned)cap * period / 10000;
                if (end > until) end = until;
                while (running && ns() < end) compute(256);
            }
        } else if (!role) {
            uint64_t release = next;
            uint64_t began = ns();
            compute(iterations); /* Fixed work, not a wall-clock-duration burn. */
            message.job = (pg_job_record){++id, release, release + PG_CRITICAL_DEADLINE, ns(), began};
        }
        next += period;
        now = ns();
        /* Report missed releases explicitly; do not erase them by moving the clock. */
        if (role && next < now) next = now;
        sleep_to(next);
    }
    name_close(coid);
    return fault ? 1 : 0;
}
