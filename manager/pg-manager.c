/* QNX bridge: pgbench telemetry and worker ACKs <-> PowerGuard policy IPC. */
#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include "ipc.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop(int sig) { (void)sig; running = 0; }

static int timed_send(int coid, const void *request, size_t bytes, void *reply, size_t capacity) {
    uint64_t timeout = PG_RESPONSE_TARGET;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL) == -1) return 0;
    return MsgSend(coid, request, bytes, reply, capacity) != -1;
}

static int bench_send(int coid, unsigned command, int arg, int extra, pg_status_t *reply) {
    pg_request_t request = {PG_BENCH_TYPE, PG_BENCH_VERSION, command, arg, extra};
    if (!timed_send(coid, &request, sizeof(request), reply, sizeof(*reply))) return 0;
    if (reply->error) { errno = reply->error; return 0; }
    return 1;
}

static int policy_send(int coid, const pg_message_v1 *request, pg_reply_v1 *reply) {
    return timed_send(coid, request, sizeof(*request), reply, sizeof(*reply));
}

static int bounded(int value) {
    return value < 0 ? 0 : value > 100 ? 100 : value;
}

int main(void) {
    int result = 0;
    int policy_coid = name_open(PG_NAME, 0);
    if (policy_coid == -1) { perror("name_open PowerGuard"); return 1; }
    int bench_coid = name_open(PG_BENCH_NAME, 0);
    if (bench_coid == -1) { perror("name_open pgbench"); name_close(policy_coid); return 1; }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    uint64_t last_bench_job = 0, reported_job = 0;
    int last_state = PG_ST_UNKNOWN, last_recovery = -1;
    puts("pg-manager connected: telemetry, caps, effects, state and jobs are bridged");
    while (running) {
        pg_status_t bench;
        if (!bench_send(bench_coid, PG_CMD_STATUS, 0, 0, &bench)) { perror("pgbench status"); result = 1; break; }
        int cpu = bounded(bench.process_cpu_per_core_tenths / 10);
        int demand2 = bench.scenario == PG_SC_SPIKE ? 90 : 15;
        pg_sample sample;
        memset(&sample, 0, sizeof(sample));
        sample.thermal_seq = bench.thermal_acquired_ns;
        sample.acquired_ns = bench.thermal_acquired_ns;
        sample.load_end_ns = bench.cpu_window_end_ns;
        sample.temp_deci_c = bench.temperature_milli_c / 100;
        sample.quality = GOOD; /* The policy ages the original acquisition timestamp. */
        sample.source = SIMULATED;
        for (unsigned i = 0; i < 4; ++i) sample.core_busy_pct[i] = cpu;
        sample.nc_demand_pct[0] = 20;
        sample.nc_demand_pct[1] = demand2;

        pg_message_v1 msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = PG_SAMPLE;
        msg.version = PG_VERSION;
        msg.body.sample = sample;
        pg_reply_v1 decision;
        if (!policy_send(policy_coid, &msg, &decision)) { perror("PowerGuard sample"); result = 1; break; }

        const pg_status *want = &decision.decision;
        pg_cap_request_t grant = {
            .request = {PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_LEASED_CAPS,
                        (int)want->nc1_pct, (int)want->nc2_pct},
            .epoch = want->epoch, .decision_id = want->decision_id,
            .until_ns = want->lease_until_ns
        };
        if (!timed_send(bench_coid, &grant, sizeof(grant), &bench, sizeof(bench)) ||
            (bench.error && bench.error != ESTALE)) {
            fputs("pgbench cap delivery failed\n", stderr); result = 1; break;
        }
        int grant_accepted = bench.error == 0;
        int recovery = want->state == RECOVERY ? (int)want->nc1_pct : 0;
        int state = pg_bench_state_from_policy((enum pg_state)want->state);
        if (state != last_state || recovery != last_recovery) {
            if (!bench_send(bench_coid, PG_CMD_STATE, state, recovery, &bench)) {
                perror("pgbench state"); result = 1; break;
            }
            last_state = state;
            last_recovery = recovery;
        }
        if (grant_accepted && bench.applied_epoch == want->epoch && bench.applied_decision_id == want->decision_id &&
            bench.nc1_cap_pct == (int)want->nc1_pct && bench.nc2_cap_pct == (int)want->nc2_pct &&
            bench.last_effective_ns >= want->issued_ns) {
            memset(&msg, 0, sizeof(msg));
            msg.type = PG_EFFECT; msg.version = PG_VERSION;
            msg.body.effect.epoch = want->epoch;
            msg.body.effect.decision_id = want->decision_id;
            msg.body.effect.effective_ns = bench.last_effective_ns;
            msg.body.effect.nc1_pct = want->nc1_pct;
            msg.body.effect.nc2_pct = want->nc2_pct;
            if (!policy_send(policy_coid, &msg, &decision) && errno != EINVAL) { perror("PowerGuard effect"); result = 1; break; }
        }
        /* The manager may attach after pgbench has already run jobs.  Treat
           that first observed ID as a baseline; later source-ID jumps remain
           visible as gaps in the supervisor scorecard. */
        if (!last_bench_job) last_bench_job = bench.last_job_id;
        else if (bench.last_job_id > last_bench_job) {
            reported_job += bench.last_job_id - last_bench_job;
            memset(&msg, 0, sizeof(msg));
            msg.type = PG_JOB; msg.version = PG_VERSION;
            msg.body.job.epoch = decision.decision.epoch;
            msg.body.job.id = reported_job;
            msg.body.job.release_ns = bench.last_job_release_ns;
            msg.body.job.deadline_ns = pg_job_budget_ns(bench.last_job_release_ns,
                                                        bench.last_job_deadline_ns);
            msg.body.job.finish_ns = bench.last_job_finish_ns;
            if (!policy_send(policy_coid, &msg, &decision)) { perror("PowerGuard job"); result = 1; break; }
            last_bench_job = bench.last_job_id;
        }

        struct timespec pause = {.tv_nsec = PG_TICK};
        while (running && nanosleep(&pause, &pause) == -1 && errno == EINTR) {}
    }
    name_close(bench_coid);
    name_close(policy_coid);
    return result;
}
