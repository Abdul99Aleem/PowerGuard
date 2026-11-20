/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * tools/pg-stall-check.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#include "ipc.h"
#include "pg_threadctl.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t manager;

static void interrupted(int sig)
{
    if (manager > 0) {
        kill(manager, SIGCONT);
    }
    _exit(128 + sig);
}

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        if (manager > 0) {
            kill(manager, SIGCONT);
        }
        _exit(1);
    }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static void pause_ms(unsigned ms)
{
    uint64_t tolerance = 1;
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * PG_MS};
    if (TimerTimeout(CLOCK_MONOTONIC, TIMER_TOLERANCE, NULL, &tolerance, NULL) == -1) {
        if (manager > 0) {
            kill(manager, SIGCONT);
        }
        _exit(1);
    }
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {
    }
}

static int read_receipt(int coid, pg_confirmation_reply *out)
{
    pg_message_v1 q = {.type = PG_CONFIRM_READ, .version = PG_VERSION};
    uint64_t timeout = 2 * PG_MS;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout,
                        NULL) != -1 &&
           MsgSend(coid, &q, sizeof(q), out, sizeof(*out)) != -1;
}

int main(int argc, char **argv)
{
    char *end;
    long pid = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (argc != 2 || !*argv[1] || *end || pid <= 1 || pid > INT32_MAX) {
        return 2;
    }
    pg_thread_report placement;
    if (pg_thread_configure(0, 18, "pg-stall-test", &placement)) {
        perror("stall test qualification");
        return 1;
    }
    int coid = name_open(PG_NAME, 0);
    if (coid == -1) {
        perror("stall test connect");
        return 1;
    }
    pg_confirmation_reply before;
    pg_confirmation_reply receipt = {0};
    if (!read_receipt(coid, &before) || before.snapshot.decision.state != NORMAL ||
        !before.manager_progress_seq) {
        return 1;
    }
    manager = (sig_atomic_t)pid;
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    uint64_t stopped = now();
    if (kill(manager, SIGSTOP)) {
        perror("stop test manager");
        manager = 0;
        return 1;
    }
    int found = 0;
    while (now() - stopped < PG_MANAGER_MAX_AGE + PG_RESPONSE_TARGET + 5 * PG_MS) {
        if (!read_receipt(coid, &receipt)) {
            break;
        }
        const pg_status *s = &receipt.snapshot.decision;
        if ((s->causes & BAD_MANAGER) && s->state == CRITICAL && !s->nc1_pct && !s->nc2_pct &&
            receipt.restriction && receipt.received_ns >= stopped &&
            receipt.received_ns >= receipt.detected_ns) {
            found = 1;
            break;
        }
        pause_ms(1);
    }
    uint64_t confirmation = found ? receipt.received_ns - receipt.detected_ns : UINT64_MAX;
    int ok = found && confirmation <= PG_RESPONSE_TARGET &&
             receipt.detected_ns == receipt.manager_progress_ns + PG_MANAGER_MAX_AGE &&
             receipt.snapshot.decision.effective_ns >= receipt.detected_ns &&
             receipt.snapshot.decision.effective_ns <= receipt.received_ns;
    printf("%s manager_stopped=1 epoch=%" PRIu64 " decision=%" PRIu64 " stop_ns=%" PRIu64
           " progress_ns=%" PRIu64 " detected_ns=%" PRIu64 " effective_ns=%" PRIu64
           " supervisor_received_ns=%" PRIu64 " confirmation_ns=%" PRIu64 "\n",
           ok ? "PASS" : "FAIL", receipt.snapshot.decision.epoch,
           receipt.snapshot.decision.decision_id, stopped, receipt.manager_progress_ns,
           receipt.detected_ns, receipt.snapshot.decision.effective_ns, receipt.received_ns,
           confirmation);

    while (now() - stopped < UINT64_C(1000000000)) {
        pause_ms(1);
    }
    if (kill(manager, SIGCONT)) {
        ok = 0;
    }
    manager = 0;
    name_close(coid);
    return ok ? 0 : 1;
}
