/* QNX bridge: pgbench telemetry and worker ACKs <-> PowerGuard policy IPC. */
#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include "ipc.h"
#include "progress.h"
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

static int bench_send(int coid, unsigned command, int arg, int extra, pg_status_t *reply) {
    pg_request_t request = {PG_BENCH_TYPE, PG_BENCH_VERSION, command, arg, extra};
    return MsgSend(coid, &request, sizeof(request), reply, sizeof(*reply)) != -1;
}

static int policy_send(int coid, const pg_message_v1 *request, pg_reply_v1 *reply) {
    return MsgSend(coid, request, sizeof(*request), reply, sizeof(*reply)) != -1;
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

    uint64_t last_acquired = 0, thermal_seq = 0, last_bench_job = 0;
    uint64_t reported_job = 0, last_decision = 0;
    const pg_progress *watchdog = pg_progress_open(0);
    int forced_off = 0;
    puts("pg-manager connected: telemetry, caps, effects, state and jobs are bridged");
    while (running) {
        pg_status_t bench;
        if (!bench_send(bench_coid, PG_CMD_STATUS, 0, 0, &bench)) { perror("pgbench status"); result = 1; break; }
        uint64_t acquired = bench.monotonic_ns - (uint64_t)bench.sensor_age_ms * PG_MS;
        if (acquired > last_acquired) { last_acquired = acquired; ++thermal_seq; }
        int cpu = bounded(bench.process_cpu_per_core_tenths / 10);
        int demand2 = bench.scenario == PG_SC_SPIKE ? 90 : 15;
        pg_sample sample;
        memset(&sample, 0, sizeof(sample));
        sample.thermal_seq = thermal_seq;
        sample.acquired_ns = acquired;
        sample.load_end_ns = bench.monotonic_ns;
        sample.temp_deci_c = bench.temperature_milli_c / 100;
        sample.quality = bench.sensor_age_ms <= 250 ? GOOD : FROZEN;
        sample.source = SIMULATED;
        sample.verified = 1; /* pgbench generates a new value for every sample */
        for (unsigned i = 0; i < 4; ++i) sample.core_busy_pct[i] = cpu;
        sample.nc_demand_pct[0] = 20;
        sample.nc_demand_pct[1] = demand2;

        pg_message_v1 msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = PG_SAMPLE_POWER;
        msg.version = PG_VERSION;
        msg.reserved = (uint32_t)cpu; /* estimated power index until a real meter is added */
        msg.body.sample = sample;
        pg_reply_v1 decision;
        if (!policy_send(policy_coid, &msg, &decision)) { perror("PowerGuard sample"); result = 1; break; }

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
            msg.body.job.deadline_ns = bench.last_job_deadline_ns;
            msg.body.job.finish_ns = bench.last_job_finish_ns;
            if (!policy_send(policy_coid, &msg, &decision)) { perror("PowerGuard job"); result = 1; break; }
            last_bench_job = bench.last_job_id;
        }

        const pg_status *want = &decision.decision;
        /* A watchdog stop is latched here too, so no restore can undo it.
           A new supervisor run ends it, and the current caps are sent again. */
        if (!watchdog) watchdog = pg_progress_open(0);
        int forced = watchdog && pg_forced_off(watchdog);
        if (forced && !forced_off &&
            !bench_send(bench_coid, PG_CMD_CAPS, 0, 0, &bench)) {
            perror("pgbench forced off"); result = 1; break;
        }
        if (!forced && forced_off) last_decision = 0;
        forced_off = forced;
        if (!forced_off && want->decision_id != last_decision) {
            if (!bench_send(bench_coid, PG_CMD_CAPS, (int)want->nc1_pct, (int)want->nc2_pct, &bench)) {
                perror("pgbench caps"); result = 1; break;
            }
            last_decision = want->decision_id;
        }
        int recovery = want->state == RECOVERY ? (int)want->nc1_pct : 0;
        if (!bench_send(bench_coid, PG_CMD_STATE,
                        pg_bench_state_from_policy((enum pg_state)want->state), recovery, &bench)) {
            perror("pgbench state"); result = 1; break;
        }
        if (bench.nc1_cap_pct == (int)want->nc1_pct && bench.nc2_cap_pct == (int)want->nc2_pct &&
            bench.last_effective_ns >= want->issued_ns) {
            memset(&msg, 0, sizeof(msg));
            msg.type = PG_EFFECT; msg.version = PG_VERSION;
            msg.body.effect.epoch = want->epoch;
            msg.body.effect.decision_id = want->decision_id;
            msg.body.effect.effective_ns = bench.last_effective_ns;
            msg.body.effect.nc1_pct = want->nc1_pct;
            msg.body.effect.nc2_pct = want->nc2_pct;
            if (!policy_send(policy_coid, &msg, &decision)) { perror("PowerGuard effect"); result = 1; break; }
        }
        struct timespec pause = {.tv_nsec = PG_TICK};
        while (running && nanosleep(&pause, &pause) == -1 && errno == EINTR) {}
    }
    name_close(bench_coid);
    name_close(policy_coid);
    return result;
}
