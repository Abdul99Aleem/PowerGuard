#define _POSIX_C_SOURCE 200809L
#include "pgbench_protocol.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/select.h>

#define NS 1000000000ULL
#define PERIOD_NS 10000000ULL
#define DEADLINE_NS 5000000ULL
#define JOB_NS 1000000ULL     /* approximate 1 ms CPU burn */
#define WORKER_NS 10000000ULL    /* 10 ms: meets the supervisor's 50 ms lease */
#define TELEMETRY_NS 10000000ULL /* 10 ms: meets the supervisor's CPU freshness */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t running = 1;
static pg_status_t st;
static uint64_t start_ns, last_sensor_ns, last_tick_ns, last_cpu_ns;
static int level = 2, scenario = PG_SC_NORMAL, current_state = PG_ST_UNKNOWN;
static int nc1_cap, nc2_cap;
static int stop_nc1_after_nc2;
static uint64_t cap_epoch, cap_id, lease_until_ns;
static uint64_t level_command_ns, ack_ns[2], recover_ns;
static uint64_t scenario_started_ns;

static uint64_t now_ns(clockid_t id) {
    struct timespec t; clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * NS + (uint64_t)t.tv_nsec;
}
static void sleep_until(uint64_t when) {
    struct timespec t = { (time_t)(when / NS), (long)(when % NS) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}
static void burn_until(uint64_t until) {
    volatile unsigned long x = 1;
    while (now_ns(CLOCK_MONOTONIC) < until) x = x * 1664525UL + 1013904223UL;
    (void)x;
}
static void *critical_worker(void *unused) {
    (void)unused;
    struct sched_param param;
    memset(&param, 0, sizeof param);
    param.sched_priority = 20;
    /* Try FIFO; continue with ordinary scheduling when permission is unavailable. */
    int fifo_ok = (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0);
    pthread_mutex_lock(&mu); st.critical_fifo_ok = fifo_ok; pthread_mutex_unlock(&mu);
    uint64_t release = now_ns(CLOCK_MONOTONIC) + PERIOD_NS;
    while (running) {
        sleep_until(release);
        if (!running) break;
        uint64_t began = now_ns(CLOCK_MONOTONIC);
        burn_until(now_ns(CLOCK_MONOTONIC) + JOB_NS);
        uint64_t ended = now_ns(CLOCK_MONOTONIC);
        uint64_t response = ended > release ? ended - release : 0;
        uint64_t jitter = began > release ? began - release : release - began;
        pthread_mutex_lock(&mu);
        st.critical_jobs++;
        st.last_job_id = st.critical_jobs;
        st.last_job_release_ns = release;
        st.last_job_deadline_ns = release + DEADLINE_NS;
        st.last_job_finish_ns = ended;
        if (ended > release + DEADLINE_NS) st.critical_misses++;
        if (response / 1000 > st.critical_max_response_us) st.critical_max_response_us = response / 1000;
        if (jitter / 1000 > st.critical_max_jitter_us) st.critical_max_jitter_us = jitter / 1000;
        release += PERIOD_NS;
        if (release < ended) {
            uint64_t skipped = (ended - release + PERIOD_NS - 1) / PERIOD_NS;
            st.critical_jobs += skipped;
            st.critical_misses += skipped;
            release += skipped * PERIOD_NS;
        }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}
/* Called under mu at a worker boundary, after the previous chunk finished. */
static int worker_cap(int id, uint64_t now) {
    if (lease_until_ns && now >= lease_until_ns) {
        /* The worker can withdraw a grant even if both services are stalled. */
        nc2_cap = nc1_cap = 0;
        lease_until_ns = 0;
        stop_nc1_after_nc2 = 0;
        level = st.level_requested = 2;
        level_command_ns = now;
        ack_ns[0] = ack_ns[1] = 0;
    }
    /* On a two-worker stop, NC2 gates first. If it stalls, NC1 still
       closes after one worker period and the pair stays unconfirmed. */
    if (stop_nc1_after_nc2 && (id == 1 || now - level_command_ns >= WORKER_NS)) {
        nc1_cap = 0;
        stop_nc1_after_nc2 = 0;
    }
    int cap = id == 0 ? nc1_cap : nc2_cap;
    if (!(id == 0 && stop_nc1_after_nc2) && ack_ns[id] < level_command_ns) {
        ack_ns[id] = now;
        if (ack_ns[0] >= level_command_ns && ack_ns[1] >= level_command_ns) {
            uint64_t latest = ack_ns[0] > ack_ns[1] ? ack_ns[0] : ack_ns[1];
            st.last_actuation_us = (latest - level_command_ns) / 1000;
            st.level_applied = level;
            st.nc1_cap_pct = nc1_cap;
            st.nc2_cap_pct = nc2_cap;
            st.last_effective_ns = latest;
            st.applied_epoch = cap_epoch;
            st.applied_decision_id = lease_until_ns ? cap_id : 0;
            if (level == 0 && recover_ns != 0) {
                st.recovery_to_full_ms = (latest - recover_ns) / 1000000;
                recover_ns = 0;
            }
        }
    }
    return cap;
}

static void *noncritical_worker(void *ptr) {
    int id = (int)(intptr_t)ptr;
    uint64_t next = now_ns(CLOCK_MONOTONIC);
    while (running) {
        pthread_mutex_lock(&mu);
        uint64_t now = now_ns(CLOCK_MONOTONIC);
        int cap = worker_cap(id, now);
        uint64_t grant_end = lease_until_ns;
        int local_scenario = scenario;
        pthread_mutex_unlock(&mu);
        /* Approximate software duty-cycle: normal NC1 20%, NC2 15%; spike NC2 90%. */
        int percent = id == 0 ? 20 : (local_scenario == PG_SC_SPIKE ? 90 : 15);
        if (cap > 0) {
            uint64_t end = now_ns(CLOCK_MONOTONIC) + (uint64_t)percent * (unsigned)cap * WORKER_NS / 10000;
            if (grant_end && end > grant_end) end = grant_end;
            burn_until(end);
        }
        next += WORKER_NS;
        uint64_t n = now_ns(CLOCK_MONOTONIC);
        if (next < n) next = n + 1000000ULL;
        sleep_until(next);
    }
    return NULL;
}
static void *sampler(void *unused) {
    (void)unused;
    while (running) {
        sleep_until(now_ns(CLOCK_MONOTONIC) + TELEMETRY_NS);
        uint64_t n = now_ns(CLOCK_MONOTONIC);
        uint64_t cpu = now_ns(CLOCK_PROCESS_CPUTIME_ID);
        pthread_mutex_lock(&mu);
        /* Explicitly SIMULATED. A frozen sample retains its old timestamp. */
        uint64_t elapsed = n - scenario_started_ns;
        if (scenario != PG_SC_FREEZE || elapsed < 4 * NS) {
            int32_t temp = 45000;
            if (scenario == PG_SC_RAMP) {
                uint64_t seconds = elapsed / NS;
                temp += (int32_t)(seconds > 35 ? 35000 : seconds * 1000);
            } else if (scenario == PG_SC_SPIKE) temp = 56000;
            else if (scenario == PG_SC_FREEZE) temp = 52000;
            st.temperature_milli_c = temp;
            last_sensor_ns = n;
        }
        if (last_tick_ns && n > last_tick_ns) {
            uint64_t delta_cpu = cpu - last_cpu_ns;
            st.process_cpu_per_core_tenths = (int32_t)(delta_cpu * 1000 / (n - last_tick_ns));
        }
        last_tick_ns = n; last_cpu_ns = cpu;
        st.sensor_age_ms = (uint32_t)((n - last_sensor_ns) / 1000000);
        st.monotonic_ns = n;
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}
static void sig_stop(int sig) { (void)sig; running = 0; }
static void snapshot(pg_status_t *dst) {
    pthread_mutex_lock(&mu);
    *dst = st;
    uint64_t n = now_ns(CLOCK_MONOTONIC);
    dst->monotonic_ns = n;
    dst->sensor_age_ms = (uint32_t)((n - last_sensor_ns) / 1000000);
    pthread_mutex_unlock(&mu);
}
static void write_csv(FILE *fp, const pg_status_t *s) {
    fprintf(fp, "%" PRIu64 ",%d,%d,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
      s->monotonic_ns, s->scenario, s->temperature_milli_c, s->sensor_age_ms,
      s->process_cpu_per_core_tenths, s->level_requested, s->level_applied,
      s->supervisor_state, s->critical_jobs, s->critical_misses,
      s->critical_max_response_us, s->critical_max_jitter_us,
      s->last_actuation_us, s->recovery_to_full_ms);
    fflush(fp);
}
static void *csv_writer(void *ptr) {
    FILE *fp = ptr;
    while (running) {
        sleep_until(now_ns(CLOCK_MONOTONIC) + NS);
        pg_status_t s; snapshot(&s); write_csv(fp, &s);
    }
    return NULL;
}
/* Called under mu. Renewing a grant preserves its original effect timestamp. */
static int apply_caps(const pg_cap_request_t *cmd, uint64_t now) {
    int nc1 = cmd->request.arg, nc2 = cmd->request.extra;
    if (nc1 < 0 || nc1 > 100 || nc2 < 0 || nc2 > 100) return EINVAL;
    if (cmd->request.command == PG_CMD_LEASED_CAPS) {
        if (!cmd->epoch || !cmd->decision_id || cmd->until_ns <= now ||
            cmd->epoch < cap_epoch || (cmd->epoch == cap_epoch && cmd->decision_id < cap_id))
            return ESTALE;
        if (cmd->epoch == cap_epoch && cmd->decision_id == cap_id) {
            int target1 = stop_nc1_after_nc2 ? 0 : nc1_cap;
            if (lease_until_ns > now && nc1 == target1 && nc2 == nc2_cap) {
                if (cmd->until_ns > lease_until_ns) lease_until_ns = cmd->until_ns;
                return 0;
            }
            /* An expired generation may only re-establish closed gates. */
            if (nc1 || nc2) return ESTALE;
        }
        cap_epoch = cmd->epoch;
        cap_id = cmd->decision_id;
        lease_until_ns = cmd->until_ns;
    } else if (cap_epoch) {
        return EBUSY;
    }

    if (!nc1_cap && !nc2_cap && (nc1 || nc2) && !recover_ns) recover_ns = now;
    stop_nc1_after_nc2 = nc1_cap && nc2_cap && !nc1 && !nc2;
    if (!stop_nc1_after_nc2) nc1_cap = nc1;
    nc2_cap = nc2;
    level = !nc1 ? 2 : !nc2 ? 1 : 0;
    st.level_requested = level;
    level_command_ns = now;
    ack_ns[0] = ack_ns[1] = 0;
    return 0;
}

static void serve(const char *csv_path) {
    name_attach_t *attach = name_attach(NULL, PG_BENCH_NAME, 0);
    if (!attach) { perror("name_attach"); exit(1); }
    FILE *fp = fopen(csv_path, "w");
    if (!fp) { perror(csv_path); name_detach(attach, 0); exit(1); }
    fprintf(fp, "monotonic_ns,scenario,temp_sim_milli_c,sensor_age_ms,process_cpu_per_core_tenths,level_requested,level_applied,supervisor_state,critical_jobs,deadline_misses,max_response_us,max_release_jitter_us,last_actuation_us,recovery_to_full_ms\n");
    start_ns = last_sensor_ns = scenario_started_ns = now_ns(CLOCK_MONOTONIC);
    st.temperature_milli_c = 45000;
    last_cpu_ns = now_ns(CLOCK_PROCESS_CPUTIME_ID);
    last_tick_ns = start_ns;
    st.supervisor_state = PG_ST_UNKNOWN;
    st.level_requested = st.level_applied = level;
    st.nc1_cap_pct = nc1_cap;
    st.nc2_cap_pct = nc2_cap;
    pthread_t critical, nc1, nc2, sample, logger;
    if (pthread_create(&critical, NULL, critical_worker, NULL) ||
        pthread_create(&nc1, NULL, noncritical_worker, (void *)(intptr_t)0) ||
        pthread_create(&nc2, NULL, noncritical_worker, (void *)(intptr_t)1) ||
        pthread_create(&sample, NULL, sampler, NULL) ||
        pthread_create(&logger, NULL, csv_writer, fp)) {
        fprintf(stderr, "thread creation failed\n"); exit(1);
    }
    printf("pgbench PID %ld: QNX name=%s; CSV=%s (simulated temperature)\n", (long)getpid(), PG_BENCH_NAME, csv_path);
    fflush(stdout);
    while (running) {
        union { pg_request_t req; pg_cap_request_t caps; struct _pulse pulse; } msg;
        memset(&msg, 0, sizeof msg);
        struct _msg_info info;
        int rcvid = MsgReceive(attach->chid, &msg, sizeof msg, &info);
        if (rcvid == -1) { if (errno == EINTR) continue; perror("MsgReceive"); break; }
        if (rcvid == 0) { if (msg.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(msg.pulse.scoid); continue; }
        if (msg.req.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        pg_status_t result;
        int err = 0;
        pthread_mutex_lock(&mu);
        size_t expected = msg.req.command == PG_CMD_LEASED_CAPS ? sizeof(msg.caps) : sizeof(msg.req);
        if (msg.req.type != PG_BENCH_TYPE || msg.req.version != PG_BENCH_VERSION ||
            info.srcmsglen != (ssize_t)expected || info.msglen != (ssize_t)expected) err = EPROTO;
        else switch (msg.req.command) {
          case PG_CMD_STATUS: break;
          case PG_CMD_SCENARIO:
            if (msg.req.arg < PG_SC_NORMAL || msg.req.arg > PG_SC_FREEZE) err = EINVAL;
            else {
                scenario = msg.req.arg; st.scenario = scenario;
                scenario_started_ns = last_sensor_ns = now_ns(CLOCK_MONOTONIC);
                st.sensor_age_ms = 0; st.temperature_milli_c = scenario == PG_SC_NORMAL ? 45000 : st.temperature_milli_c;
            }
            break;
          case PG_CMD_LEVEL:
            if (cap_epoch) { err = EBUSY; break; }
            if (msg.req.arg < 0 || msg.req.arg > 2) err = EINVAL;
            else {
                int old = level;
                stop_nc1_after_nc2 = 0;
                level = msg.req.arg; st.level_requested = level;
                nc1_cap = level < 2 ? 100 : 0;
                nc2_cap = level < 1 ? 100 : 0;
                level_command_ns = now_ns(CLOCK_MONOTONIC);
                ack_ns[0] = ack_ns[1] = 0;
                if (old == 2 && level < 2 && recover_ns == 0) recover_ns = level_command_ns;
            }
            break;
          case PG_CMD_LEASED_CAPS:
          case PG_CMD_CAPS:
            err = apply_caps(&msg.caps, now_ns(CLOCK_MONOTONIC));
            break;
          case PG_CMD_STATE:
            if (msg.req.arg < PG_ST_UNKNOWN || msg.req.arg > PG_ST_RECOVERY ||
                msg.req.extra < 0 || msg.req.extra > 100) err = EINVAL;
            else { current_state = msg.req.arg; st.supervisor_state = current_state; st.recovery_percent = msg.req.extra; }
            break;
          case PG_CMD_STOP: running = 0; break;
          default: err = EINVAL; break;
        }
        result = st;
        result.thermal_acquired_ns = last_sensor_ns;
        result.cpu_window_end_ns = last_tick_ns;
        result.monotonic_ns = now_ns(CLOCK_MONOTONIC);
        result.sensor_age_ms = (uint32_t)((result.monotonic_ns - last_sensor_ns) / 1000000);
        result.error = err;
        pthread_mutex_unlock(&mu);
        MsgReply(rcvid, EOK, &result, sizeof result);
    }
    name_detach(attach, 0);
    /* Threads wake at most once per second; SIGINT can interrupt MsgReceive. */
    pthread_join(critical, NULL); pthread_join(nc1, NULL); pthread_join(nc2, NULL);
    pthread_join(sample, NULL); pthread_join(logger, NULL);
    fclose(fp);
}
static void print_status(const pg_status_t *s) {
    const char *names[] = {"normal", "ramp", "spike", "freeze"};
    const char *states[] = {"UNKNOWN", "NORMAL", "WARNING", "THROTTLED", "CRITICAL", "RECOVERY"};
    printf("Scenario: %s | temperature: %.1f C (SIMULATED), age: %u ms\n",
      names[s->scenario], s->temperature_milli_c / 1000., s->sensor_age_ms);
    printf("CPU: %.1f%% of one core (process CPU time) | NC1 cap: %d%% | NC2 cap: %d%%\n",
      s->process_cpu_per_core_tenths / 10., s->nc1_cap_pct, s->nc2_cap_pct);
    printf("Restriction: requested=%d applied=%d | supervisor=%s | recovery=%d%%\n",
      s->level_requested, s->level_applied, states[s->supervisor_state], s->recovery_percent);
    printf("Critical: jobs=%" PRIu64 " misses=%" PRIu64 " max_response=%" PRIu64 " us max_jitter=%" PRIu64 " us FIFO=%s\n",
      s->critical_jobs, s->critical_misses, s->critical_max_response_us,
      s->critical_max_jitter_us, s->critical_fifo_ok ? "YES" : "NO (fallback)");
    printf("Last requested-level-to-worker-ACK=%" PRIu64 " us | recovery-to-full=%" PRIu64 " ms\n",
      s->last_actuation_us, s->recovery_to_full_ms);
}
static void usage(const char *argv0) {
    fprintf(stderr, "Usage:\n  %s serve [csv_file]\n  %s status\n  %s dashboard\n  %s scenario normal|ramp|spike|freeze\n  %s level 0|1|2\n  %s state unknown|normal|warning|throttled|critical|recovery [progress_0_to_100]\n  %s stop\n", argv0,argv0,argv0,argv0,argv0,argv0,argv0);
}
static int dashboard_send(int coid, unsigned command, int arg, pg_status_t *reply) {
    pg_request_t req = {PG_BENCH_TYPE, PG_BENCH_VERSION, command, arg, 0};
    if (MsgSend(coid, &req, sizeof(req), reply, sizeof(*reply)) == -1) { perror("MsgSend"); return 0; }
    if (reply->error) { fprintf(stderr, "pgbench error: %s\n", strerror(reply->error)); return 0; }
    return 1;
}
static void dashboard(int coid) {
    static const char *names[] = {"normal", "ramp", "spike", "freeze"};
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) == -1) { perror("tcgetattr (use a terminal)"); return; }
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) { perror("tcsetattr"); return; }
    int selected = -1;
    for (;;) {
        pg_status_t reply;
        if (!dashboard_send(coid, PG_CMD_STATUS, 0, &reply)) break;
        if (selected < 0) selected = reply.scenario;
        printf("\033[2J\033[HPOWERGUARD LIVE DASHBOARD\nUp/Down: change active scenario | q: quit\n\n");
        print_status(&reply);
        printf("\nActive scenario: %s\n", names[reply.scenario]);
        fflush(stdout);
        fd_set ready;
        FD_ZERO(&ready); FD_SET(STDIN_FILENO, &ready);
        struct timeval refresh = {.tv_sec = 0, .tv_usec = 500000};
        int available = select(STDIN_FILENO + 1, &ready, NULL, NULL, &refresh);
        if (available < 0) { if (errno == EINTR) continue; perror("select"); break; }
        if (available == 0) continue; /* Automatic status refresh twice per second. */
        int c = getchar();
        if (c == 'q' || c == 'Q') break;
        int next = selected;
        if (c == 27 && getchar() == '[') {
            c = getchar();
            if (c == 'A' && selected < PG_SC_FREEZE) ++next;
            else if (c == 'B' && selected > PG_SC_NORMAL) --next;
        }
        if (next != selected) {
            selected = next;
            (void)dashboard_send(coid, PG_CMD_SCENARIO, selected, &reply);
        }
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    puts("\033[2J\033[H");
}
int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "serve")) {
        signal(SIGINT, sig_stop); signal(SIGTERM, sig_stop);
        serve(argc >= 3 ? argv[2] : "pgbench.csv");
        return 0;
    }
    if (argc < 2) { usage(argv[0]); return 2; }
    if (!strcmp(argv[1], "dashboard")) {
        int coid = name_open(PG_BENCH_NAME, 0);
        if (coid == -1) { perror("name_open (is pgbench serve running?)"); return 1; }
        dashboard(coid);
        name_close(coid);
        return 0;
    }
    pg_request_t req = { PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_STATUS, 0, 0 };
    if (!strcmp(argv[1], "status")) {}
    else if (!strcmp(argv[1], "stop")) req.command = PG_CMD_STOP;
    else if (!strcmp(argv[1], "level") && argc >= 3) {
        req.command = PG_CMD_LEVEL; req.arg = atoi(argv[2]);
    } else if (!strcmp(argv[1], "scenario") && argc >= 3) {
        const char *options[] = {"normal","ramp","spike","freeze"};
        int i; for (i=0; i<4; ++i) if (!strcmp(argv[2],options[i])) break;
        if (i == 4) { usage(argv[0]); return 2; }
        req.command = PG_CMD_SCENARIO; req.arg = i;
    } else if (!strcmp(argv[1], "state") && argc >= 3) {
        const char *options[] = {"unknown","normal","warning","throttled","critical","recovery"};
        int i; for (i=0; i<6; ++i) if (!strcmp(argv[2],options[i])) break;
        if (i == 6) { usage(argv[0]); return 2; }
        req.command = PG_CMD_STATE; req.arg = i;
        if (argc >= 4) req.extra = atoi(argv[3]);
    } else { usage(argv[0]); return 2; }
    int coid = name_open(PG_BENCH_NAME, 0);
    if (coid == -1) { perror("name_open (is pgbench serve running?)"); return 1; }
    pg_status_t reply;
    memset(&reply, 0, sizeof reply);
    int rc = MsgSend(coid, &req, sizeof req, &reply, sizeof reply);
    name_close(coid);
    if (rc == -1) { perror("MsgSend"); return 1; }
    if (reply.error) { fprintf(stderr, "pgbench error: %s\n", strerror(reply.error)); return 1; }
    print_status(&reply);
    return 0;
}
