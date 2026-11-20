/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * .milestone-handoff.inc
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#include <devctl.h>
#include <sys/procfs.h>
#include <sched.h>

static int thread_sample(pid_t pid, unsigned tid, procfs_status *s)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/as", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    memset(s, 0, sizeof *s);
    s->tid = tid;
    int err = devctl(fd, DCMD_PROC_TIDSTATUS, s, sizeof *s, NULL);
    close(fd);
    return !err && (unsigned)s->tid == tid;
}

static void print_trace(const char *file)
{
    FILE *fp = fopen(file, "r");
    if (!fp) {
        perror(file);
        return;
    }
    char line[256];
    printf("BEGIN %s\n", file);
    while (fgets(line, sizeof line, fp)) {
        fputs(line, stdout);
    }
    printf("END %s\n", file);
    fclose(fp);
}

static int run_handoff(pg_reply_v1 *policy)
{
    const pid_t pid[] = {bench, bench, bench, bench, bench, bench, supervisor, manager};
    const unsigned tid[] = {1, 2, 3, 4, 5, 6, 1, 1};
    const char *role[] = {"bench IPC", "critical", "NC1",        "NC2",
                          "sampler",   "logger",   "supervisor", "manager"};
    procfs_status first[8];
    procfs_status last[8];
    pg_status_t b;
    if (bench_command(PG_CMD_STATUS, 0, &b) < 0) {
        return 2;
    }
    uint64_t begin = b.monotonic_ns;
    uint64_t decision = policy->decision.decision_id;
    for (unsigned i = 0; i < 8; i++) {
        if (!thread_sample(pid[i], tid[i], &first[i])) {
            return 2;
        }
    }
    for (int second = 0; second < 10; second++) {
        pause_ms(1000);
        if (read_policy(policy) < 0 || policy->decision.state != NORMAL ||
            policy->decision.decision_id != decision) {
            puts("Normal hold failed");
            return 1;
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        if (!thread_sample(pid[i], tid[i], &last[i])) {
            return 2;
        }
    }
    if (bench_command(PG_CMD_STATUS, 0, &b) < 0) {
        return 2;
    }
    uint64_t elapsed = b.monotonic_ns - begin;
    double subtotal = 0;
    printf("Normal epoch=%" PRIu64 " decision=%" PRIu64 " elapsed_ns=%" PRIu64 " caps=%d/%d\n",
           (uint64_t)policy->decision.epoch, decision, elapsed, b.nc1_cap_pct, b.nc2_cap_pct);
    for (unsigned i = 0; i < 8; i++) {
        double cost = 100.0 * (last[i].sutime - first[i].sutime) / elapsed;
        printf("Role=%s tid=%u policy=%s priority=%u last_cpu=%u cpu_percent=%.4f\n", role[i],
               tid[i],
               last[i].policy == SCHED_FIFO ? "FIFO"
               : last[i].policy == SCHED_RR ? "RR"
                                            : "other",
               last[i].real_priority, last[i].last_cpu, cost);
        if (i == 0 || i == 4 || i >= 6) {
            subtotal += cost;
        }
    }
    printf("Bench IPC + sampler + supervisor + manager subtotal=%.4f percent of one core.\n",
           subtotal);
    int ok = b.nc1_cap_pct == 100 && b.nc2_cap_pct == 100 && b.critical_fifo_ok &&
             last[1].last_cpu == 0 && last[2].last_cpu == 2 && last[3].last_cpu == 3;
    stop();
    if (check_jobs(begin, begin + elapsed)) {
        ok = 0;
    }
    print_trace("worker-effects.csv");
    print_trace("critical-jobs.csv");
    struct stat st;
    if (stat("/proc/ham/.info", &st) == 0 || errno != ENOENT) {
        return 2;
    }
    puts("HAM postflight: /proc/ham/.info absent.");
    printf("Placement and deadline check=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
