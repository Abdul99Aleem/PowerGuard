/* Independent watchdog (PRD page 2). Every 50 ms it reads the supervisor's
   progress counter from shared memory. After 200 ms without progress it
   latches forced-off and tells pgbench to stop both workers itself, without
   waiting for the supervisor or the manager. The latch lasts for the rest of
   that supervisor run; a new supervisor epoch is a new run. */
#include "progress.h"
#include "pgbench_protocol.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>

static uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

/* Bounded send: a stuck pgbench must not stall the watchdog's checks. */
static int bench_send(int coid, unsigned command, int arg, int extra, pg_status_t *reply) {
    pg_request_t request = {PG_BENCH_TYPE, PG_BENCH_VERSION, command, arg, extra};
    uint64_t timeout = 20 * PG_MS;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL) == -1) return 0;
    return MsgSend(coid, &request, sizeof(request), reply, sizeof(*reply)) != -1;
}

int main(int argc, char **argv) {
    long priority = 44;
    if (argc > 2) { fprintf(stderr, "usage: pg-watchdog [FIFO_PRIORITY]\n"); return 2; }
    if (argc == 2) {
        char *end;
        priority = strtol(argv[1], &end, 10);
        if (*end || priority < sched_get_priority_min(SCHED_FIFO) ||
            priority > sched_get_priority_max(SCHED_FIFO)) return 2;
    }
    struct sched_param sched = {.sched_priority = (int)priority};
    if (sched_setscheduler(0, SCHED_FIFO, &sched) == -1) { perror("SCHED_FIFO"); return 1; }

    pg_progress *shared = pg_progress_open(1);
    if (!shared) { perror("progress shm"); return 1; }

    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int coid = chid == -1 ? -1 : ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) { perror("channel"); return 1; }
    struct sigevent event;
    SIGEV_PULSE_INIT(&event, coid, (int)priority, _PULSE_CODE_MINAVAIL, 0);
    timer_t timer;
    struct itimerspec tolerance = {.it_value = {.tv_nsec = 1}};
    struct itimerspec period = {.it_value = {.tv_nsec = PG_WATCHDOG_PERIOD},
                                .it_interval = {.tv_nsec = PG_WATCHDOG_PERIOD}};
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) == -1 ||
        timer_settime(timer, TIMER_TOLERANCE, &tolerance, NULL) == -1 ||
        timer_settime(timer, 0, &period, NULL) == -1) { perror("watchdog timer"); return 1; }

    int bench = name_open(PG_BENCH_NAME, 0);
    if (bench == -1) perror("name_open pgbench (will retry)");
    puts("pg-watchdog: checking supervisor progress every 50 ms");
    fflush(stdout);

    /* The record outlives a run, so only arm once the counter has moved. */
    uint64_t first = pg_load(&shared->progress), forced_ns = 0;
    int armed = 0, sent = 0, reported = 0;
    for (;;) {
        struct _pulse pulse;
        if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) == -1) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse"); return 1;
        }
        uint64_t now = monotonic_ns();
        if (!armed && pg_load(&shared->progress) == first) continue; /* no supervisor yet */
        armed = 1;
        if (forced_ns && pg_load(&shared->epoch) > forced_ns) {
            forced_ns = 0; /* a new supervisor run starts the watch afresh */
            sent = reported = 0;
        }
        uint64_t last = pg_load(&shared->progress_ns);
        if (!forced_ns && now - last >= PG_WATCHDOG_STALL) {
            forced_ns = now;
            pg_store(&shared->stalled_at_ns, last);
            pg_store(&shared->forced_off_ns, forced_ns);
            printf("pg-watchdog: no progress for %.1f ms, both workers forced off\n",
                   ms(now - last));
            fflush(stdout);
        }
        if (!forced_ns) continue;

        /* Keep trying until pgbench takes the stop, then report when both
           workers confirmed it. The latch itself never clears here. */
        pg_status_t bench_status;
        if (bench == -1) bench = name_open(PG_BENCH_NAME, 0);
        if (bench == -1) continue;
        if (!sent) sent = bench_send(bench, PG_CMD_CAPS, 0, 0, &bench_status);
        else if (!reported && bench_send(bench, PG_CMD_STATUS, 0, 0, &bench_status) &&
                 bench_status.nc1_cap_pct == 0 && bench_status.nc2_cap_pct == 0 &&
                 bench_status.last_effective_ns >= forced_ns) {
            printf("pg-watchdog: both off %.1f ms after the last progress (limit 270 ms)\n",
                   ms(bench_status.last_effective_ns - pg_load(&shared->stalled_at_ns)));
            fflush(stdout);
            reported = 1;
        }
    }
}
