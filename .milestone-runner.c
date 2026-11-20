/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * .milestone-runner.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#define _POSIX_C_SOURCE 200809L
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ipc.h"
#include "pgbench_protocol.h"
extern char **environ;
#include "assets.h"
#ifdef CLI_TRIAL
#include "cli-assets.h"
static pid_t cli = -1;
static int cli_input = -1;
#endif
static pid_t bench = -1;
static pid_t supervisor = -1;
static pid_t manager = -1;
static char folder[] = "/tmp/pg-t3-baseline-XXXXXX";

static void pause_ms(long ms)
{
    struct timespec delay = {ms / 1000, ms % 1000 * 1000000};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

static int request(const char *name, const void *msg, size_t len, void *reply, size_t rlen)
{
    uint64_t ns = 100000000;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &ns, NULL);
    int coid = name_open(name, 0);
    if (coid < 0) {
        return -1;
    }
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &ns, NULL);
    int rc = MsgSend(coid, msg, len, reply, rlen);
    name_close(coid);
    return rc;
}

static int read_policy(pg_reply_v1 *p)
{
    pg_message_v1 m = {.type = PG_READ, .version = PG_VERSION};
    return request(PG_NAME, &m, sizeof m, p, sizeof *p);
}

static int bench_command(unsigned command, int arg, pg_status_t *p)
{
    pg_request_t m = {PG_BENCH_TYPE, PG_BENCH_VERSION, command, arg, 0};
    return request(PG_BENCH_NAME, &m, sizeof m, p, sizeof *p);
}

static int stage(const char *name, const unsigned char *p, size_t n)
{
    int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0700);
    if (fd < 0) {
        return -1;
    }
    while (n) {
        ssize_t size = write(fd, p, n);
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size <= 0) {
            close(fd);
            return -1;
        }
        p += size;
        n -= (size_t)size;
    }
    return close(fd);
}

static pid_t launch(char **argv)
{
    pid_t p;
    int rc = posix_spawn(&p, argv[0], NULL, NULL, argv, environ);
    if (rc) {
        errno = rc;
        perror(argv[0]);
        return -1;
    }
    return p;
}

static void reap(pid_t p)
{
    if (p <= 0) {
        return;
    }
    for (int i = 0; i < 25; i++) {
        if (waitpid(p, NULL, WNOHANG) != 0) {
            return;
        }
        pause_ms(100);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

static void stop(void)
{
    pg_status_t b;
#ifdef CLI_TRIAL
    if (cli_input >= 0) {
        close(cli_input);
        cli_input = -1;
    }
    if (cli > 0) {
        kill(cli, SIGCONT);
        kill(cli, SIGTERM);
        reap(cli);
        cli = -1;
    }
#endif
    if (manager > 0) {
        kill(manager, SIGTERM);
    }
    if (bench > 0 && bench_command(PG_CMD_STOP, 0, &b) < 0) {
        kill(bench, SIGTERM);
    }
    if (supervisor > 0) {
        kill(supervisor, SIGTERM);
    }
    reap(bench);
    reap(manager);
    reap(supervisor);
    bench = manager = supervisor = -1;
}

static int check_jobs(uint64_t begin, uint64_t end)
{
    FILE *f = fopen("critical-jobs.csv", "r");
    if (!f) {
        perror("critical-jobs.csv");
        return 2;
    }
    char line[200];
    uint64_t release;
    uint64_t start;
    uint64_t finish;
    uint64_t previous = 0;
    uint64_t max_response = 0;
    uint64_t num = 0;
    uint64_t miss = 0;
    uint64_t gaps = 0;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 2;
    }
    while (fscanf(f, "%" SCNu64 ",%" SCNu64 ",%" SCNu64 "\n", &release, &start, &finish) == 3) {
        if (release < begin || release + 5000000 > end) {
            continue;
        }
        if (previous && release != previous + 10000000) {
            ++gaps;
        }
        if (!previous && release - begin >= 10000000) {
            ++gaps;
        }
        previous = release;
        ++num;
        if (!finish || finish < start || start < release) {
            ++miss;
            continue;
        }
        uint64_t response = finish - release;
        if (response > 5000000) {
            ++miss;
        }
        if (response > max_response) {
            max_response = response;
        }
    }
    fclose(f);
    if (!previous || end - previous >= 15000000) {
        ++gaps;
    }
    printf("Critical trace: releases=%" PRIu64 " misses=%" PRIu64 " gaps=%" PRIu64
           " max_response_ns=%" PRIu64 "\n",
           num, miss, gaps, max_response);
    return num && !miss && !gaps ? 0 : 1;
}
#ifdef CRITICAL_TRIAL
#ifdef STALE_TRIAL
static uint64_t last_valid_ns;
#endif
#ifdef FTTI_TRIAL
#define FTTI_BUDGET_NS (250 * PG_MS)
static uint64_t ftti_onset_ns;

static int check_ftti_timing(uint64_t issued, uint64_t nc2, uint64_t nc1, uint64_t confirmed)
{
    FILE *f = fopen("ftti-ramp.csv", "r");
    if (!f) {
        perror("ftti-ramp.csv");
        return 0;
    }
    uint64_t onset;
    uint64_t first;
    uint64_t reached_80;
    int32_t first_temp;
    int fields = fscanf(f, "%" SCNu64 ",%" SCNu64 ",%" SCNd32 ",%" SCNu64, &onset, &first,
                        &first_temp, &reached_80);
    fclose(f);
    if (fields != 4 || !onset || first < onset || first - onset >= FTTI_BUDGET_NS ||
        first_temp != 75000 + (int32_t)((first - onset) / 50000) || reached_80 < onset ||
        reached_80 - onset < FTTI_BUDGET_NS || issued < first || nc2 < issued || nc1 < nc2 ||
        confirmed < nc1) {
        puts("FTTI ramp or worker timestamps failed validation.");
        return 0;
    }
    ftti_onset_ns = onset;
    uint64_t stopped = nc1 - onset;
    uint64_t confirmed_elapsed = confirmed - onset;
    printf("FTTI ramp onset=%" PRIu64 " ns, first sample=%" PRId32 " mC at %" PRIu64
           " ns, 80 C observed at %" PRIu64 " ns after onset.\n",
           onset, first_temp, first - onset, reached_80 - onset);
    printf("FTTI timeline from ramp onset: Critical=%" PRIu64 " ns, NC2 stopped=%" PRIu64
           " ns, NC1 stopped=%" PRIu64 " ns, both caps confirmed by %" PRIu64 " ns.\n",
           issued - onset, nc2 - onset, stopped, confirmed_elapsed);
    printf("FTTI margin: worker-stop margin=%" PRId64 " ns, confirmed-cap margin=%" PRId64
           " ns against 250 ms.\n",
           (int64_t)FTTI_BUDGET_NS - (int64_t)stopped,
           (int64_t)FTTI_BUDGET_NS - (int64_t)confirmed_elapsed);
    return stopped <= FTTI_BUDGET_NS && confirmed_elapsed <= FTTI_BUDGET_NS;
}
#endif
static int check_worker_order(uint64_t epoch, uint64_t decision, uint64_t issued,
                              uint64_t effective)
{
    FILE *f = fopen("worker-effects.csv", "r");
    if (!f) {
        perror("worker-effects.csv");
        return 0;
    }
    char line[256];
    uint64_t row_epoch;
    uint64_t row_decision;
    uint64_t stamp;
    uint64_t closed[2] = {0, 0};
    unsigned count[2] = {0, 0};
    int worker;
    int cap;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    while (fscanf(f, "%" SCNu64 ",%" SCNu64 ",%d,%d,%" SCNu64 "\n", &row_epoch, &row_decision,
                  &worker, &cap, &stamp) == 5) {
        if (row_epoch != epoch || row_decision != decision || cap != 0 || worker < 1 ||
            worker > 2) {
            continue;
        }
        ++count[worker - 1];
        closed[worker - 1] = stamp;
    }
    fclose(f);
    if (count[0] != 1 || count[1] != 1 || closed[0] < issued || closed[1] < issued) {
        puts("Missing or ambiguous zero-cap worker records.");
        return 0;
    }
    printf("NC2 gate closed=%" PRIu64 " ns, decision-to-NC2=%" PRIu64 " ns\n", closed[1],
           closed[1] - issued);
    printf("NC1 gate closed=%" PRIu64 " ns, decision-to-NC1=%" PRIu64 " ns\n", closed[0],
           closed[0] - issued);
    if (closed[1] >= closed[0] || closed[0] != effective) {
        puts("Worker order or effect join failed.");
        return 0;
    }
#ifdef STALE_TRIAL
    if (!last_valid_ns || issued < last_valid_ns) {
        return 0;
    }
    uint64_t critical_age = issued - last_valid_ns;
    uint64_t nc2_age = closed[1] - last_valid_ns;
    uint64_t nc1_age = closed[0] - last_valid_ns;
    printf("Last valid temperature sample=%" PRIu64 " ns.\n", last_valid_ns);
    printf("Stale input timeline from last valid sample: Critical=%" PRIu64
           " ns, NC2 stopped=%" PRIu64 " ns, NC1 stopped=%" PRIu64 " ns.\n",
           critical_age, nc2_age, nc1_age);
    int stale_pass = critical_age >= PG_TEMP_MAX_AGE && critical_age < PG_TEMP_MAX_AGE + PG_MS &&
                     nc1_age <= PG_TEMP_MAX_AGE + PG_RESPONSE_TARGET;
    printf("Stale input timing=%s (150 ms freshness threshold, 170 ms gate deadline).\n",
           stale_pass ? "PASS" : "FAIL");
#endif
    printf("NC2 preceded NC1 by %" PRIu64 " ns.\n", closed[0] - closed[1]);
    f = fopen("manager-confirmation.csv", "r");
    if (!f) {
        perror("manager-confirmation.csv");
        return 0;
    }
    int read = fscanf(f, "%" SCNu64 ",%" SCNu64 ",%" SCNu64, &row_epoch, &row_decision, &stamp);
    fclose(f);
    if (read != 3 || row_epoch != epoch || row_decision != decision || stamp < effective) {
        return 0;
    }
    printf("Supervisor accepted both caps by %" PRIu64
           " ns, decision-to-confirmation upper bound=%" PRIu64 " ns\n",
           stamp, stamp - issued);
#ifdef STALE_TRIAL
    printf("Both caps confirmed by %" PRIu64 " ns after the last valid sample.\n",
           stamp - last_valid_ns);
    return stale_pass && stamp - issued <= PG_RESPONSE_TARGET;
#elif defined(FTTI_TRIAL)
    return check_ftti_timing(issued, closed[1], closed[0], stamp) &&
           stamp - issued <= PG_RESPONSE_TARGET;
#else
    return stamp - issued <= 20000000;
#endif
}
#endif
#ifdef RECOVERY_TRIAL
typedef struct {
    uint64_t epoch;
    uint64_t started_ns;
    uint64_t clear_ns;
    uint64_t nc2_clear_ns;
    uint64_t nc1_decision;
    uint64_t nc2_decision;
    uint64_t nc1_issue;
    uint64_t nc2_issue;
} recovery_trial;

static int read_stack(pg_reply_v1 *policy, pg_status_t *workers)
{
    return read_policy(policy) >= 0 && bench_command(PG_CMD_STATUS, 0, workers) >= 0 &&
           !workers->error;
}

static int wait_for_gated_clear(uint64_t epoch, uint64_t *clear_ns)
{
    pg_reply_v1 p;
    pg_status_t b;
    for (int i = 0; i < 100; i++) {
        if (read_stack(&p, &b) && p.decision.epoch == epoch && p.decision.state == RECOVERY &&
            p.decision.clear_since_ns && !p.decision.nc1_pct && !p.decision.nc2_pct &&
            !b.nc1_cap_pct && !b.nc2_cap_pct) {
            *clear_ns = p.decision.clear_since_ns;
            return 1;
        }
        pause_ms(10);
    }
    return 0;
}

static int run_recovery(recovery_trial *trial, uint64_t *end)
{
    pg_reply_v1 p;
    pg_status_t b;
    uint64_t first_clear;
    if (bench_command(PG_CMD_SCENARIO, PG_SC_NORMAL, &b) < 0 || b.error ||
        !wait_for_gated_clear(trial->epoch, &first_clear)) {
        puts("Initial clear period did not start with both workers gated.");
        return 0;
    }
    printf("Initial clear period began at %" PRIu64 " ns with caps 0/0.\n", first_clear);
    pause_ms(600);
    if (bench_command(PG_CMD_SCENARIO, PG_SC_SPIKE, &b) < 0 || b.error) {
        return 0;
    }
    int reset = 0;
    for (int i = 0; i < 100; i++) {
        if (read_stack(&p, &b) && p.decision.epoch == trial->epoch &&
            p.decision.state == CRITICAL && (p.decision.causes & POWER) &&
            !p.decision.clear_since_ns && !b.nc1_cap_pct && !b.nc2_cap_pct) {
            printf("Interrupted clear period reset after %" PRIu64 " ns, with caps still 0/0.\n",
                   p.decision.as_of_ns - first_clear);
            reset = 1;
            break;
        }
        pause_ms(10);
    }
    if (!reset) {
        puts("Recovery did not reset on the test interruption.");
        return 0;
    }
    if (bench_command(PG_CMD_SCENARIO, PG_SC_NORMAL, &b) < 0 || b.error ||
        !wait_for_gated_clear(trial->epoch, &trial->clear_ns) || trial->clear_ns <= first_clear) {
        puts("Fresh clear period did not start after the interruption.");
        return 0;
    }
    printf("Fresh clear period began at %" PRIu64 " ns.\n", trial->clear_ns);
    for (int i = 0; i < 500; i++) {
        pause_ms(10);
        if (!read_stack(&p, &b)) {
            continue;
        }
        const pg_status *s = &p.decision;
        if (s->epoch != trial->epoch || (s->state != RECOVERY && s->state != NORMAL)) {
            puts("Unexpected state or epoch after the clear period restarted.");
            return 0;
        }
        if (!trial->nc1_decision && s->state == RECOVERY && s->nc1_pct == 100 && !s->nc2_pct &&
            s->effective_ns && s->clear_since_ns && b.applied_epoch == s->epoch &&
            b.applied_decision_id == s->decision_id && b.nc1_cap_pct == 100 && !b.nc2_cap_pct) {
            trial->nc1_decision = s->decision_id;
            trial->nc1_issue = s->issued_ns;
            trial->nc2_clear_ns = s->clear_since_ns;
            printf("NC1-only stage confirmed, decision=%" PRIu64 ", caps=100/0.\n",
                   trial->nc1_decision);
        }
        if (s->state == NORMAL && s->effective_ns && b.applied_epoch == s->epoch &&
            b.applied_decision_id == s->decision_id && b.nc1_cap_pct == 100 &&
            b.nc2_cap_pct == 100) {
            if (!trial->nc1_decision || !b.critical_fifo_ok) {
                return 0;
            }
            trial->nc2_decision = s->decision_id;
            trial->nc2_issue = s->issued_ns;
            pause_ms(200);
            if (!read_stack(&p, &b) || p.decision.epoch != trial->epoch ||
                p.decision.state != NORMAL || b.nc1_cap_pct != 100 || b.nc2_cap_pct != 100 ||
                !b.critical_fifo_ok) {
                return 0;
            }
            *end = b.monotonic_ns;
            printf("Returned to Normal, decision=%" PRIu64 ", caps=100/100, FIFO=1.\n",
                   trial->nc2_decision);
            return 1;
        }
    }
    puts("Recovery did not finish in the test window.");
    return 0;
}

static int check_recovery_effects(const recovery_trial *trial)
{
    FILE *f = fopen("worker-effects.csv", "r");
    if (!f) {
        perror("worker-effects.csv");
        return 0;
    }
    char line[256];
    uint64_t row_epoch;
    uint64_t decision;
    uint64_t stamp;
    uint64_t reopened[2] = {0, 0};
    uint64_t generation[2] = {0, 0};
    int worker;
    int cap;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    while (fscanf(f, "%" SCNu64 ",%" SCNu64 ",%d,%d,%" SCNu64 "\n", &row_epoch, &decision, &worker,
                  &cap, &stamp) == 5) {
        if (row_epoch != trial->epoch || stamp < trial->started_ns || worker < 1 || worker > 2 ||
            !cap) {
            continue;
        }
        if (!reopened[worker - 1]) {
            reopened[worker - 1] = stamp;
            generation[worker - 1] = decision;
        }
    }
    fclose(f);
    if (generation[0] != trial->nc1_decision || generation[1] != trial->nc2_decision ||
        trial->nc1_issue < trial->clear_ns + PG_DWELL || trial->nc2_clear_ns < reopened[0] ||
        trial->nc2_issue < trial->nc2_clear_ns + PG_RESTORE_DWELL ||
        reopened[0] < trial->nc1_issue || reopened[1] < trial->nc2_issue) {
        puts("Unexpected worker reopening order, decision, or dwell.");
        return 0;
    }
    printf("NC1 reopened after %" PRIu64 " ns of continuous clear.\n",
           reopened[0] - trial->clear_ns);
    printf("NC2 reopened %" PRIu64 " ns after NC1 and %" PRIu64
           " ns after the second dwell started.\n",
           reopened[1] - reopened[0], reopened[1] - trial->nc2_clear_ns);
    printf("Both caps restored %" PRIu64 " ns after the fresh clear period began.\n",
           reopened[1] - trial->clear_ns);
    return 1;
}
#endif
#ifdef WORKER_FAULT_TRIAL
enum { DELAY_NC2_ACK = 1, DISCONNECT_NC2 = 2, MANAGER_LOSS = 3 };

static int worker_fault;

static int check_fault_effects(uint64_t epoch, uint64_t decision, uint64_t issued)
{
    FILE *f = fopen("worker-effects.csv", "r");
    if (!f) {
        perror("worker-effects.csv");
        return 0;
    }
    char line[256];
    uint64_t row_epoch;
    uint64_t row_decision;
    uint64_t stamp;
    uint64_t closed[2] = {0, 0};
    int worker;
    int cap;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    while (fscanf(f, "%" SCNu64 ",%" SCNu64 ",%d,%d,%" SCNu64 "\n", &row_epoch, &row_decision,
                  &worker, &cap, &stamp) == 5) {
        if (row_epoch == epoch && row_decision == decision && !cap && worker >= 1 && worker <= 2) {
            closed[worker - 1] = stamp;
        }
    }
    fclose(f);
    if (closed[0] < issued) {
        puts("Missing actual NC1 stop record.");
        return 0;
    }
    printf("NC1 fallback gate closed %" PRIu64 " ns after the decision.\n", closed[0] - issued);
    if (worker_fault == DISCONNECT_NC2) {
        printf("Disconnected NC2 gate ACK present=%s.\n", closed[1] ? "yes" : "no");
        return !closed[1];
    }
    if (closed[1] <= closed[0]) {
        puts("NC2 late acknowledgement was missing or not after the fallback.");
        return 0;
    }
    printf("Delayed NC2 gate ACK arrived %" PRIu64 " ns after the decision.\n", closed[1] - issued);
    return closed[1] - issued > PG_RESPONSE_TARGET;
}

static int run_worker_fault(const pg_status *normal)
{
    pg_status_t b;
    pg_reply_v1 p;
    pg_request_t change = {PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_SCENARIO, PG_SC_FREEZE,
                           worker_fault};
    if (request(PG_BENCH_NAME, &change, sizeof change, &b, sizeof b) < 0 || b.error) {
        return 1;
    }
    uint64_t begin = b.monotonic_ns;
    uint64_t decision = 0;
    uint64_t issued = 0;
    for (int i = 0; i < 1400; i++) {
        pause_ms(5);
        if (read_policy(&p) < 0) {
            continue;
        }
        if (p.decision.state == CRITICAL &&
            (p.decision.causes & (BAD_TEMP | WAIT_EFFECT)) == (BAD_TEMP | WAIT_EFFECT) &&
            !p.decision.nc1_pct && !p.decision.nc2_pct && !p.decision.effective_ns) {
            decision = p.decision.decision_id;
            issued = p.decision.issued_ns;
            printf("Missed effect flagged after %" PRIu64
                   " ns. Requested caps=0/0, pair unconfirmed.\n",
                   p.decision.as_of_ns - issued);
            break;
        }
    }
    if (!decision || decision == normal->decision_id) {
        puts("Expected missing effect was not observed.");
        return 1;
    }
    pg_message_v1 stale = {.type = PG_EFFECT, .version = PG_VERSION};
    stale.body.effect.epoch = normal->epoch;
    stale.body.effect.decision_id = normal->decision_id;
    stale.body.effect.effective_ns = normal->effective_ns;
    stale.body.effect.nc1_pct = normal->nc1_pct;
    stale.body.effect.nc2_pct = normal->nc2_pct;
    int reply = request(PG_NAME, &stale, sizeof stale, &p, sizeof p);
    if (reply != -1 || errno != EINVAL) {
        puts("Obsolete acknowledgement was not rejected with EINVAL.");
        return 1;
    }
    printf("Obsolete acknowledgement for decision %" PRIu64 " rejected with EINVAL.\n",
           normal->decision_id);
    pause_ms(250);
    if (read_policy(&p) < 0 || bench_command(PG_CMD_STATUS, 0, &b) < 0 || b.error ||
        p.decision.state != CRITICAL || p.decision.epoch != normal->epoch ||
        p.decision.decision_id != decision || p.decision.nc1_pct || p.decision.nc2_pct ||
        !b.critical_fifo_ok) {
        puts("Critical, safe requested caps, or FIFO did not hold.");
        return 1;
    }
    int expected;
    if (worker_fault == DISCONNECT_NC2) {
        expected = !p.decision.effective_ns && (p.decision.causes & WAIT_EFFECT);
        printf("After disconnect: Critical, requested caps=0/0, pair confirmed=%s.\n",
               p.decision.effective_ns ? "yes" : "no");
    } else {
        expected = p.decision.effective_ns > issued + PG_RESPONSE_TARGET &&
                   b.applied_epoch == normal->epoch && b.applied_decision_id == decision &&
                   !b.nc1_cap_pct && !b.nc2_cap_pct;
        printf("After delayed ACK: Critical, confirmed zero-cap pair=%s.\n",
               expected ? "yes" : "no");
    }
    uint64_t end = b.monotonic_ns;
    stop();
    int jobs = check_jobs(begin, end);
    int effects = check_fault_effects(normal->epoch, decision, issued);
    struct stat ham_info;
    int ham_off = stat("/proc/ham/.info", &ham_info) != 0 && errno == ENOENT;
    printf("HAM postflight: %s.\n",
           ham_off ? "/proc/ham/.info absent" : "HAM absence not verified");
    int passed = jobs == 0 && effects && expected && ham_off;
    printf("Worker fault result=%s.\n", passed ? "PASS" : "FAIL");
    return !passed;
}

static int check_lease_expiry(uint64_t epoch, uint64_t stopped)
{
    FILE *f = fopen("lease-expiry.csv", "r");
    if (!f) {
        perror("lease-expiry.csv");
        return 0;
    }
    uint64_t expiry;
    uint64_t expired_at;
    int entries = fscanf(f, "%" SCNu64 ",%" SCNu64, &expiry, &expired_at);
    fclose(f);
    if (entries != 2 || expiry < stopped || expired_at < expiry) {
        return 0;
    }
    f = fopen("worker-effects.csv", "r");
    if (!f) {
        perror("worker-effects.csv");
        return 0;
    }
    char line[256];
    uint64_t row_epoch;
    uint64_t decision;
    uint64_t stamp;
    uint64_t closed[2] = {0, 0};
    int worker;
    int cap;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    while (fscanf(f, "%" SCNu64 ",%" SCNu64 ",%d,%d,%" SCNu64 "\n", &row_epoch, &decision, &worker,
                  &cap, &stamp) == 5) {
        if (row_epoch == epoch && !decision && !cap && stamp >= expired_at && worker >= 1 &&
            worker <= 2 && !closed[worker - 1]) {
            closed[worker - 1] = stamp;
        }
    }
    fclose(f);
    if (!closed[0] || !closed[1]) {
        puts("Local lease-expiry gate records missing.");
        return 0;
    }
    uint64_t last = closed[0] > closed[1] ? closed[0] : closed[1];
    printf("Local lease expired %" PRIu64 " ns after stop request. NC1 closed %" PRIu64
           " ns and NC2 %" PRIu64 " ns after the expiry.\n",
           expiry - stopped, closed[0] - expiry, closed[1] - expiry);
    return last - expiry <= PG_RESPONSE_TARGET;
}

static int run_manager_loss(const pg_status *normal)
{
    pg_status_t b;
    pg_reply_v1 p;
    if (bench_command(PG_CMD_STATUS, 0, &b) < 0 || b.error) {
        return 1;
    }
    uint64_t begin = b.monotonic_ns;
    pid_t old_manager = manager;
    if (kill(old_manager, SIGTERM) != 0) {
        perror("stop test manager");
        return 1;
    }
    reap(old_manager);
    manager = -1;
    int gated = 0;
    for (int i = 0; i < 100; i++) {
        pause_ms(5);
        if (read_policy(&p) >= 0 && bench_command(PG_CMD_STATUS, 0, &b) >= 0 && !b.error &&
            p.decision.state == CRITICAL && (p.decision.causes & WAIT_EFFECT) &&
            !p.decision.effective_ns && b.applied_epoch == normal->epoch &&
            !b.applied_decision_id && !b.nc1_cap_pct && !b.nc2_cap_pct) {
            gated = 1;
            break;
        }
    }
    if (!gated) {
        puts("Both worker gates did not close on local lease expiry.");
        return 1;
    }
    printf(
        "Manager PID %ld stopped. Workers reached caps 0/0 locally while the supervisor awaited an effect.\n",
        (long)old_manager);
    pause_ms(150);
    char *args[] = {"./pg-manager", NULL};
    manager = launch(args);
    if (manager < 0) {
        return 1;
    }
    uint64_t fresh_decision = 0;
    int resumed = 0;
    for (int i = 0; i < 500; i++) {
        pause_ms(10);
        if (read_policy(&p) < 0 || bench_command(PG_CMD_STATUS, 0, &b) < 0 || b.error) {
            continue;
        }
        const pg_status *s = &p.decision;
        if (s->epoch != normal->epoch || !b.critical_fifo_ok) {
            break;
        }
        if (!fresh_decision && s->decision_id > normal->decision_id && s->effective_ns &&
            !s->nc1_pct && !s->nc2_pct && b.applied_decision_id == s->decision_id &&
            !b.nc1_cap_pct && !b.nc2_cap_pct) {
            fresh_decision = s->decision_id;
            printf("Restarted manager PID %ld confirmed fresh zero-cap decision %" PRIu64 ".\n",
                   (long)manager, fresh_decision);
        }
        if (fresh_decision && s->state == NORMAL && s->decision_id > fresh_decision &&
            s->effective_ns && b.applied_decision_id == s->decision_id && b.nc1_cap_pct == 100 &&
            b.nc2_cap_pct == 100) {
            puts("After manager restart: Normal confirmed, caps=100/100.");
            resumed = 1;
            break;
        }
    }
    if (!resumed) {
        puts("The restarted manager did not restore confirmed Normal.");
        return 1;
    }
    uint64_t end = b.monotonic_ns;
    stop();
    int jobs = check_jobs(begin, end);
    int expiry = check_lease_expiry(normal->epoch, begin);
    struct stat ham_info;
    int ham_off = stat("/proc/ham/.info", &ham_info) != 0 && errno == ENOENT;
    printf("HAM postflight: %s.\n",
           ham_off ? "/proc/ham/.info absent" : "HAM absence not verified");
    int passed = jobs == 0 && expiry && ham_off;
    printf("Manager loss result=%s.\n", passed ? "PASS" : "FAIL");
    return !passed;
}
#endif
#ifdef CLI_TRIAL
static const char *const state_names[] = {"Normal", "Warning", "Throttled", "Critical", "Recovery"};

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int send_cli_line(const char *line)
{
    size_t length = strlen(line);
    return write(cli_input, line, length) == (ssize_t)length;
}

static off_t cli_log_size(void)
{
    struct stat result;
    return stat("cli-output.txt", &result) == 0 ? result.st_size : -1;
}

static int start_cli(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return 0;
    }
    posix_spawn_file_actions_t files;
    int error = posix_spawn_file_actions_init(&files);
    if (!error) {
        error = posix_spawn_file_actions_adddup2(&files, fds[0], STDIN_FILENO);
        if (!error) {
            error = posix_spawn_file_actions_addclose(&files, fds[0]);
        }
        if (!error) {
            error = posix_spawn_file_actions_addclose(&files, fds[1]);
        }
        if (!error) {
            error = posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "cli-output.txt",
                                                     O_WRONLY | O_CREAT | O_EXCL, 0600);
        }
        char *args[] = {"./pg-console", "--live", NULL};
        if (!error) {
            error = posix_spawn(&cli, args[0], &files, NULL, args, environ);
        }
        posix_spawn_file_actions_destroy(&files);
    }
    close(fds[0]);
    if (error) {
        close(fds[1]);
        errno = error;
        perror("launch live CLI");
        return 0;
    }
    cli_input = fds[1];
    signal(SIGPIPE, SIG_IGN);
    if (!send_cli_line("scores\n")) {
        return 0;
    }
    for (int i = 0; i < 50; i++) {
        pause_ms(20);
        if (cli_log_size() > 0) {
            puts("Live CLI connected and produced output.");
            return 1;
        }
    }
    return 0;
}

static int pause_cli(uint64_t *begin, uint64_t *end)
{
    if (kill(cli, SIGSTOP) != 0) {
        return 0;
    }
    int status;
    pid_t observed = 0;
    for (int i = 0; i < 50 && observed == 0; i++) {
        observed = waitpid(cli, &status, WUNTRACED | WNOHANG);
        if (!observed) {
            pause_ms(10);
        }
    }
    if (observed != cli || !WIFSTOPPED(status)) {
        return 0;
    }
    *begin = monotonic_ns();
    off_t before = cli_log_size();
    if (!*begin || before <= 0 || !send_cli_line("scores\n")) {
        return 0;
    }
    pause_ms(1200);
    *end = monotonic_ns();
    int stalled = cli_log_size() == before;
    if (kill(cli, SIGCONT) != 0 || !stalled || *end <= *begin) {
        return 0;
    }
    for (int i = 0; i < 50; i++) {
        pause_ms(20);
        if (cli_log_size() > before) {
            printf("CLI paused for %" PRIu64
                   " ns with a queued score request and resumed output.\n",
                   *end - *begin);
            return 1;
        }
    }
    return 0;
}

static int close_cli(uint64_t *begin)
{
    if (!send_cli_line("quit\n")) {
        return 0;
    }
    close(cli_input);
    cli_input = -1;
    for (int i = 0; i < 50; i++) {
        int status;
        pid_t done = waitpid(cli, &status, WNOHANG);
        if (done == cli) {
            cli = -1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                return 0;
            }
            *begin = monotonic_ns();
            puts("Live CLI closed cleanly and its process exited.");
            return *begin != 0;
        }
        if (done < 0 && errno != EINTR) {
            return 0;
        }
        pause_ms(20);
    }
    return 0;
}

static int run_cli_trial(void)
{
    if (!start_cli()) {
        return 1;
    }
    pg_status_t b;
    if (bench_command(PG_CMD_SCENARIO, PG_SC_RAMP, &b) < 0 || b.error) {
        return 1;
    }
    unsigned seen = 0;
    uint64_t paused_from = 0;
    uint64_t paused_until = 0;
    uint64_t closed_from = 0;
    uint64_t closed_until = 0;
    for (int i = 0; i < 490; i++) {
        pause_ms(100);
        pg_reply_v1 p;
        if (read_policy(&p) < 0 || p.decision.state >= sizeof state_names / sizeof state_names[0]) {
            continue;
        }
        unsigned state = p.decision.state;
        unsigned bit = 1u << state;
        int returned = state == NORMAL && (seen & (1u << RECOVERY));
        if ((seen & bit) && !returned) {
            continue;
        }
        seen |= bit;
        printf("Supervisor state %s\n", state_names[state]);
        if (state == WARNING && !pause_cli(&paused_from, &paused_until)) {
            puts("CLI pause test failed.");
            return 1;
        }
        if (state == THROTTLED && !close_cli(&closed_from)) {
            puts("CLI close test failed.");
            return 1;
        }
        if (state == CRITICAL &&
            (bench_command(PG_CMD_SCENARIO, PG_SC_NORMAL, &b) < 0 || b.error)) {
            return 1;
        }
        if (returned) {
            pause_ms(250);
            closed_until = monotonic_ns();
            break;
        }
    }
    if (seen != 31 || !paused_until || !closed_from || closed_until <= closed_from) {
        printf("CLI checks incomplete, state mask=0x%x.\n", seen);
        return 1;
    }
    stop();
    puts("Paused CLI window:");
    int paused_jobs = check_jobs(paused_from, paused_until);
    puts("Closed CLI window:");
    int closed_jobs = check_jobs(closed_from, closed_until);
    struct stat ham_info;
    int ham_off = stat("/proc/ham/.info", &ham_info) != 0 && errno == ENOENT;
    printf("HAM postflight: %s.\n",
           ham_off ? "/proc/ham/.info absent" : "HAM absence not verified");
    int passed = !paused_jobs && !closed_jobs && ham_off;
    printf("CLI-only result=%s.\n", passed ? "PASS" : "FAIL");
    return !passed;
}
#endif
#ifdef CPU_SPIKE_TRIAL
static int check_spike_effect(uint64_t epoch, uint64_t decision)
{
    FILE *f = fopen("real-cpu-spike.csv", "r");
    if (!f) {
        perror("real-cpu-spike.csv");
        return 0;
    }
    uint64_t sample_epoch;
    uint64_t sample_decision;
    uint64_t detected;
    uint64_t window;
    uint64_t issued;
    int cpu[4];
    int fields = fscanf(
        f, "%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%d,%d,%d,%d", &sample_epoch,
        &sample_decision, &detected, &window, &issued, &cpu[0], &cpu[1], &cpu[2], &cpu[3]);
    fclose(f);
    if (fields != 9 || sample_epoch != epoch || sample_decision != decision || cpu[3] < 85 ||
        issued < detected) {
        puts("Missing or mismatched measured CPU decision.");
        return 0;
    }
    printf("Detection sample: CPU0=%d%% CPU1=%d%% CPU2=%d%% CPU3=%d%%, window=%" PRIu64 " ns.\n",
           cpu[0], cpu[1], cpu[2], cpu[3], window);
    f = fopen("worker-effects.csv", "r");
    if (!f) {
        perror("worker-effects.csv");
        return 0;
    }
    char line[256];
    int worker;
    int cap;
    uint64_t row_epoch;
    uint64_t row_decision;
    uint64_t stamp;
    uint64_t nc2_stop = 0;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    while (fscanf(f, "%" SCNu64 ",%" SCNu64 ",%d,%d,%" SCNu64 "\n", &row_epoch, &row_decision,
                  &worker, &cap, &stamp) == 5) {
        if (row_epoch == epoch && row_decision == decision && worker == 2 && cap == 0 &&
            !nc2_stop) {
            nc2_stop = stamp;
        }
    }
    fclose(f);
    if (nc2_stop < issued) {
        puts("No matching NC2 gate closure after the decision.");
        return 0;
    }
    printf("NC2 gate closed %" PRIu64 " ns after the measured threshold crossing and %" PRIu64
           " ns after the decision.\n",
           nc2_stop - detected, nc2_stop - issued);
    return nc2_stop - detected <= PG_RESPONSE_TARGET;
}

static int run_spike_trial(const pg_reply_v1 *normal)
{
    const int32_t *cpu = normal->last_input.core_busy_pct;
    printf(
        "Before spike: CPU0=%d%% CPU1=%d%% CPU2=%d%% CPU3=%d%%. Temperature simulated, CPU measured on board.\n",
        cpu[0], cpu[1], cpu[2], cpu[3]);
    pg_status_t b;
    if (bench_command(PG_CMD_SCENARIO, PG_SC_SPIKE, &b) < 0 || b.error) {
        return 1;
    }
    puts("Started real NC2 compute spike at 93% requested duty.");
    uint64_t begin = b.monotonic_ns;
    uint64_t end = 0;
    uint64_t decision = 0;
    uint64_t observed = 0;
    pg_reply_v1 p;
    for (int i = 0; i < 600; i++) {
        pause_ms(2);
        if (read_policy(&p) < 0 || bench_command(PG_CMD_STATUS, 0, &b) < 0 || b.error) {
            continue;
        }
        const pg_status *s = &p.decision;
        if (s->state == CRITICAL) {
            printf("Unexpected Critical during CPU spike, causes=0x%x, per-core=%d/%d/%d/%d.\n",
                   s->causes, p.last_input.core_busy_pct[0], p.last_input.core_busy_pct[1],
                   p.last_input.core_busy_pct[2], p.last_input.core_busy_pct[3]);
            return 1;
        }
        if (s->state == THROTTLED && (s->causes & CPU) && !observed) {
            observed = s->decision_id;
        }
        if (!observed || s->decision_id != observed || s->epoch != normal->decision.epoch ||
            !s->effective_ns || b.applied_decision_id != observed || b.applied_epoch != s->epoch ||
            b.nc1_cap_pct != 100 || b.nc2_cap_pct != 0) {
            continue;
        }
        decision = observed;
        printf("CPU throttling confirmed: epoch=%" PRIu64 ", decision=%" PRIu64 ", caps=100/0.\n",
               s->epoch, decision);
        pause_ms(1000);
        if (read_policy(&p) < 0 || bench_command(PG_CMD_STATUS, 0, &b) < 0 || b.error ||
            b.nc1_cap_pct != 100 || b.nc2_cap_pct != 0 || !b.critical_fifo_ok) {
            puts("Post-spike hold or FIFO check failed.");
            return 1;
        }
        const int32_t *after = p.last_input.core_busy_pct;
        printf("After NC2 stopped: CPU0=%d%% CPU1=%d%% CPU2=%d%% CPU3=%d%%, caps=100/0.\n",
               after[0], after[1], after[2], after[3]);
        end = b.monotonic_ns;
        break;
    }
    if (!decision) {
        if (read_policy(&p) >= 0) {
            printf("No matched CPU throttling. State=%u, causes=0x%x, readings=%d/%d/%d/%d.\n",
                   p.decision.state, p.decision.causes, p.last_input.core_busy_pct[0],
                   p.last_input.core_busy_pct[1], p.last_input.core_busy_pct[2],
                   p.last_input.core_busy_pct[3]);
        }
        return 1;
    }
    stop();
    int timely = check_spike_effect(normal->decision.epoch, decision);
    int jobs = check_jobs(begin, end);
    struct stat ham_info;
    int ham_off = stat("/proc/ham/.info", &ham_info) != 0 && errno == ENOENT;
    printf("HAM postflight: %s.\n",
           ham_off ? "/proc/ham/.info absent" : "HAM absence not verified");
    int passed = timely && !jobs && ham_off;
    printf("CPU spike result=%s.\n", passed ? "PASS" : "FAIL");
    return !passed;
}
#endif
#ifdef OVERHEAD_TRIAL
#include "overhead-trial.inc"
#endif
#ifdef HANDOFF_TRIAL
#include ".milestone-handoff.inc"
#endif
static int run_trial(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct stat ham_info;
#ifdef HAM_ON_TRIAL
    if (stat("/proc/ham/.info", &ham_info) != 0) {
        perror("HAM preflight");
        return 2;
    }
    puts("HAM preflight: /proc/ham/.info present.");
#else
    if (stat("/proc/ham/.info", &ham_info) == 0) {
        fputs(
            "Baseline stopped: HAM is running. Existing HAM and its entities were left untouched.\n",
            stderr);
        return 2;
    }
    if (errno != ENOENT) {
        perror("checking /proc/ham/.info");
        return 2;
    }
    puts("HAM preflight: /proc/ham/.info absent.");
#endif
    pg_status_t b;
    pg_reply_v1 p;
    if (bench_command(PG_CMD_STATUS, 0, &b) >= 0 || read_policy(&p) >= 0) {
        fputs("Baseline stopped: a PowerGuard or pgbench service already owns its channel.\n",
              stderr);
        return 2;
    }
    if (!mkdtemp(folder) || chdir(folder)) {
        perror("run directory");
        return 2;
    }
    printf("Trial evidence directory=%s\n", folder);
    if (stage("pgbench", asset_bench, sizeof asset_bench) ||
        stage("PowerGuard", asset_supervisor, sizeof asset_supervisor) ||
        stage("pg-manager", asset_manager, sizeof asset_manager) ||
        stage("pg-check", asset_check, sizeof asset_check)) {
        perror("stage");
        return 2;
    }
#ifdef CLI_TRIAL
    if (stage("pg-console", asset_console, sizeof asset_console)) {
        perror("stage CLI");
        return 2;
    }
#endif
#ifdef OVERHEAD_TRIAL
    if (!prepare_overhead()) {
        return 2;
    }
#endif
    char *check[] = {"./pg-check", NULL};
    int rc = 0;
    pid_t check_pid = launch(check);
    if (check_pid < 0 || waitpid(check_pid, &rc, 0) < 0 || !WIFEXITED(rc) || WEXITSTATUS(rc)) {
        return 2;
    }
    puts("Policy check exited 0.");
    atexit(stop);
    char *bench_args[] = {"./pgbench", "serve", "bench.csv", NULL};
    char *service_args[] = {"./PowerGuard", "19", NULL};
    char *manager_args[] = {"./pg-manager", NULL};
#if defined(CPU_SPIKE_TRIAL) || defined(HANDOFF_TRIAL) || defined(OVERHEAD_TRIAL)
    if (setenv("PG_T3_CPU_SPIKE", "1", 1) != 0) {
        perror("set CPU measurement mode");
        return 2;
    }
#endif
    int ready = 0;
    if ((bench = launch(bench_args)) < 0) {
        return 2;
    }
    for (int i = 0; i < 40; i++) {
        pause_ms(100);
        if (bench_command(PG_CMD_STATUS, 0, &b) >= 0) {
            ready = 1;
            break;
        }
    }
    if (!ready) {
        puts("pgbench channel unavailable.");
        return 2;
    }
    printf("Initial applied caps %d/%d. Critical FIFO=%u\n", b.nc1_cap_pct, b.nc2_cap_pct,
           (unsigned)b.critical_fifo_ok);
    if ((supervisor = launch(service_args)) < 0) {
        return 2;
    }
    ready = 0;
    for (int i = 0; i < 40; i++) {
        pause_ms(100);
        if (read_policy(&p) >= 0) {
            ready = 1;
            break;
        }
    }
    if (!ready) {
        puts("Supervisor channel unavailable.");
        return 2;
    }
#ifdef OVERHEAD_TRIAL
    if (!start_aux()) {
        return 2;
    }
#endif
    if ((manager = launch(manager_args)) < 0) {
        return 2;
    }
    ready = 0;
    for (int i = 0; i < 100; i++) {
        pause_ms(100);
        if (read_policy(&p) >= 0 && p.decision.state == NORMAL && p.decision.effective_ns &&
            bench_command(PG_CMD_STATUS, 0, &b) >= 0 && b.nc1_cap_pct == 100 &&
            b.nc2_cap_pct == 100) {
            ready = 1;
            break;
        }
    }
#ifdef OVERHEAD_TRIAL
    if (!ready) {
        printf("Sampler reads=%u sent=%u received=%u skipped=%u watchdog=%u trips=%u causes=%u\n",
               stats->reads, stats->sent, stats->received, stats->skipped, stats->checks,
               stats->trips, p.decision.causes);
    }
#endif
    if (!ready) {
        printf("Normal not confirmed: state=%u caps=%d/%d jobs=%" PRIu64 " misses=%" PRIu64 "\n",
               p.decision.state, b.nc1_cap_pct, b.nc2_cap_pct, (uint64_t)b.critical_jobs,
               (uint64_t)b.critical_misses);
        return 2;
    }
#ifdef CPU_SPIKE_TRIAL
    return run_spike_trial(&p);
#endif
#ifdef OVERHEAD_TRIAL
    return watchdog_fault ? measure_watchdog_fault() : measure_overhead(&p);
#endif
#ifdef HANDOFF_TRIAL
    return run_handoff(&p);
#endif
#ifdef CLI_TRIAL
    return run_cli_trial();
#endif
#ifdef WORKER_FAULT_TRIAL
    pg_status normal = p.decision;
    return worker_fault == MANAGER_LOSS ? run_manager_loss(&normal) : run_worker_fault(&normal);
#endif
#ifdef CRITICAL_TRIAL
#ifdef FTTI_TRIAL
    printf("NORMAL confirmed, caps=100/100, FIFO=%u. Begin simulated 75 to 80 C ramp at 20 C/s.\n",
           (unsigned)b.critical_fifo_ok);
    pg_request_t ramp = {PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_SCENARIO, PG_SC_RAMP, 1};
    if (request(PG_BENCH_NAME, &ramp, sizeof ramp, &b, sizeof b) < 0 || b.error) {
        return 2;
    }
    const unsigned critical_cause = TEMP;
#else
    printf("NORMAL confirmed, caps=100/100, FIFO=%u. Begin stale-temperature injection.\n",
           (unsigned)b.critical_fifo_ok);
    if (bench_command(PG_CMD_SCENARIO, PG_SC_FREEZE, &b) < 0 || b.error) {
        return 2;
    }
    const unsigned critical_cause = BAD_TEMP;
#endif
#else
    printf("NORMAL confirmed. FIFO=%u. Begin ramp.\n", (unsigned)b.critical_fifo_ok);
    if (bench_command(PG_CMD_SCENARIO, PG_SC_RAMP, &b) < 0 || b.error) {
        return 2;
    }
#endif
    uint64_t first_ns = b.monotonic_ns;
    uint64_t last_ns = 0;
    int restriction_pass = 0;
    int confirmed = 0;
#ifdef CRITICAL_TRIAL
    uint64_t epoch = 0;
    uint64_t decision = 0;
    uint64_t issued_critical = 0;
    uint64_t effect_critical = 0;
#endif
    for (int i = 0; i < 320; i++) {
        pause_ms(100);
        if (read_policy(&p) < 0 || bench_command(PG_CMD_STATUS, 0, &b) < 0) {
            continue;
        }
#ifdef CRITICAL_TRIAL
        if (p.decision.state != NORMAL && p.decision.state != CRITICAL) {
            printf("Unexpected intermediate state=%u causes=%u\n", p.decision.state,
                   p.decision.causes);
            return 1;
        }
        if (p.decision.state == CRITICAL && (p.decision.causes & critical_cause) &&
            p.decision.effective_ns && b.applied_epoch == p.decision.epoch &&
            b.applied_decision_id == p.decision.decision_id && b.nc1_cap_pct == 0 &&
            b.nc2_cap_pct == 0) {
            epoch = p.decision.epoch;
            decision = p.decision.decision_id;
            issued_critical = p.decision.issued_ns;
            effect_critical = p.decision.effective_ns;
#ifdef STALE_TRIAL
            last_valid_ns = p.last_input.acquired_ns;
#endif
            printf("CRITICAL: epoch=%" PRIu64 " decision=%" PRIu64
                   " actual_caps=%d/%d causes=0x%x\n",
                   epoch, decision, b.nc1_cap_pct, b.nc2_cap_pct, p.decision.causes);
#ifdef FTTI_TRIAL
            pause_ms(500);
#else
            pause_ms(250);
#endif
            if (read_policy(&p) < 0 || bench_command(PG_CMD_STATUS, 0, &b) < 0 ||
                p.decision.state != CRITICAL || p.decision.epoch != epoch ||
                p.decision.decision_id != decision || b.nc1_cap_pct || b.nc2_cap_pct ||
                !b.critical_fifo_ok) {
                puts("Critical hold or FIFO check failed.");
                return 1;
            }
#ifdef STALE_TRIAL
            if (p.last_input.acquired_ns != last_valid_ns) {
                puts("Temperature acquisition timestamp changed after the input freeze.");
                return 1;
            }
#endif
            printf("Critical held with caps 0/0 and FIFO=1. Decision issued=%" PRIu64
                   " ns, effective=%" PRIu64 " ns\n",
                   issued_critical, effect_critical);
            last_ns = b.monotonic_ns;
            restriction_pass =
                effect_critical >= issued_critical && effect_critical - issued_critical <= 20000000;
            confirmed = 1;
            break;
        }
#else
        if (p.decision.state == THROTTLED && p.decision.effective_ns &&
            b.applied_epoch == p.decision.epoch &&
            b.applied_decision_id == p.decision.decision_id && b.nc1_cap_pct == 100 &&
            b.nc2_cap_pct == 0) {
            uint64_t effect = p.decision.effective_ns;
            uint64_t issued = p.decision.issued_ns;
            printf("THROTTLED: epoch=%" PRIu64 " decision=%" PRIu64 " actual_caps=%d/%d\n",
                   (uint64_t)p.decision.epoch, (uint64_t)p.decision.decision_id, b.nc1_cap_pct,
                   b.nc2_cap_pct);
            printf("Issued=%" PRIu64 " effective=%" PRIu64 " restriction_us=%" PRIu64 "\n", issued,
                   effect, (effect - issued) / 1000);
            printf("Critical bench totals: jobs=%" PRIu64 " misses=%" PRIu64
                   " max_response_us=%" PRIu64 " FIFO=%u\n",
                   (uint64_t)b.critical_jobs, (uint64_t)b.critical_misses,
                   (uint64_t)b.critical_max_response_us, (unsigned)b.critical_fifo_ok);
            printf("Scored time window: %" PRIu64 " through %" PRIu64 "\n", first_ns,
                   (uint64_t)b.monotonic_ns);
            last_ns = b.monotonic_ns;
            restriction_pass = effect - issued <= 20000000;
            confirmed = 1;
            break;
        }
#endif
    }
    if (!confirmed) {
        printf("No matched target effect. Last state=%u caps=%d/%d causes=%u\n", p.decision.state,
               b.nc1_cap_pct, b.nc2_cap_pct, p.decision.causes);
        return 1;
    }
#ifdef RECOVERY_TRIAL
    recovery_trial recovery = {.epoch = epoch, .started_ns = last_ns};
    uint64_t recovery_end = 0;
    int recovery_pass = run_recovery(&recovery, &recovery_end);
    if (recovery_end) {
        last_ns = recovery_end;
    }
#endif
    stop();
#ifdef CRITICAL_TRIAL
    restriction_pass =
        check_worker_order(epoch, decision, issued_critical, effect_critical) && restriction_pass;
#endif
#ifdef FTTI_TRIAL
    if (ftti_onset_ns) {
        first_ns = ftti_onset_ns;
    }
#endif
    int result = check_jobs(first_ns, last_ns);
#ifdef RECOVERY_TRIAL
    restriction_pass = recovery_pass && check_recovery_effects(&recovery) && restriction_pass;
#endif
#ifdef HAM_ON_TRIAL
    if (stat("/proc/ham/.info", &ham_info) != 0) {
        perror("HAM postflight");
        return 2;
    }
    puts("HAM postflight: /proc/ham/.info present.");
#else
    if (stat("/proc/ham/.info", &ham_info) == 0 || errno != ENOENT) {
        fputs("HAM absence could not be confirmed after the run.\n", stderr);
        return 2;
    }
    puts("HAM postflight: /proc/ham/.info absent.");
#endif
    printf("Trial result=%s\n", restriction_pass && result == 0 ? "PASS" : "FAIL");
    return restriction_pass && result == 0 ? 0 : 1;
}

#ifdef OVERHEAD_TRIAL
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--watchdog")) {
        watchdog_fault = 1;
        smoke = 1;
        mode = 'B';
        return run_trial();
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--smoke"))) {
        return 2;
    }
    smoke = argc == 2 && !strcmp(argv[1], "--smoke");
    const char *order = smoke ? "BCA" : "ABCBCACAB";
    int errors = 0;
    for (size_t i = 0; order[i]; i++) {
        mode = order[i];
        printf("Trial %zu mode %c, replay, warm-up %d s, scored %d s.\n", i + 1, mode,
               smoke ? 0 : 30, smoke ? 2 : 60);
        pid_t child = fork();
        if (child < 0) {
            return 2;
        }
        if (!child) {
            exit(run_trial());
        }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
            ++errors;
        }
        if (errors) {
            break;
        }
    }
    printf("Suite completed with %d acquisition errors.\n", errors);
    return !!errors;
}
#else
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef WORKER_FAULT_TRIAL
    int failed = 0;
    static const char *const names[] = {"Delayed NC2 acknowledgement", "Disconnected NC2 worker",
                                        "Manager loss and recovery"};
    for (int mode = DELAY_NC2_ACK; mode <= MANAGER_LOSS; ++mode) {
        printf("=== %s ===\n", names[mode - DELAY_NC2_ACK]);
        pid_t child = fork();
        if (child < 0) {
            perror("starting worker fault trial");
            return 2;
        }
        if (!child) {
            worker_fault = mode;
            return run_trial();
        }
        int status;
        pid_t done;
        do {
            done = waitpid(child, &status, 0);
        } while (done < 0 && errno == EINTR);
        if (done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed = 1;
        }
    }
    printf("Worker faults suite=%s.\n", failed ? "FAIL" : "PASS");
    return failed;
#else
    return run_trial();
#endif
}

#endif
