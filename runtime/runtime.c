#include "protocol.h"
#include "ipc.h"
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
int pg_worker_main(unsigned role, unsigned iterations);
#define JOB_RING 2048u
#define TICK_NS UINT64_C(10000000)
static volatile sig_atomic_t running = 1;
static pg_status_t status;
static pg_job_record jobs[JOB_RING];
static pid_t children[3];
static uint64_t worker_epoch[3], worker_id[3], worker_effect[3];
static int worker_cap[3], desired[2];
static uint64_t epoch, decision_id, grant_until, issued, watchdog_at, scenario_at, clear_at;
static uint64_t last_sample, last_cpu, idle_prev[4];
static uint64_t idle_history[4][11], time_history[11], cpu_samples;
static uint64_t dropped[3];
static clockid_t idle_clocks[4];
static int fault_latched;
static pid_t enforcer_pid;
static void stop(int sig) { (void)sig; running = 0; }
static uint64_t ns_clock(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t now(void) { return ns_clock(CLOCK_MONOTONIC); }
static int send_timed(int coid, const void *message, size_t size, void *reply, size_t capacity) {
    uint64_t timeout = UINT64_C(5000000);
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                        NULL, &timeout, NULL) != -1 &&
           MsgSend(coid, message, size, reply, capacity) != -1;
}
static int positive(const char *s, unsigned *out) {
    char *end; errno = 0;
    if (!s || *s < '0' || *s > '9') return 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || !n || n > 100000000) return 0;
    *out = (unsigned)n; return 1;
}
static int idle_setup(char **args) {
    int fd = open("/proc/1/as", O_RDONLY);
    if (fd == -1) return 0;
    unsigned tids[4];
    for (unsigned cpu = 0; cpu < 4; ++cpu) {
        if (!positive(args[cpu], &tids[cpu])) { close(fd); errno = EINVAL; return 0; }
        for (unsigned other = 0; other < cpu; ++other)
            if (tids[other] == tids[cpu]) { close(fd); errno = EINVAL; return 0; }
        procfs_status thread = {.tid = tids[cpu]};
        int e = devctl(fd, DCMD_PROC_TIDSTATUS, &thread, sizeof(thread), NULL);
        struct { int new_name_len, name_buf_len; char name_buf[64]; } label = {-1, 64, {0}};
        char expected[64]; snprintf(expected, sizeof(expected), "idle_cpu_%u", cpu);
        int named = ThreadCtlExt(1, (int)tids[cpu], _NTO_TCTL_NAME, &label);
        label.name_buf[sizeof(label.name_buf)-1] = 0;
        /* This QNX 8 Pi image reports idle base priority 1. Verify identity too. */
        if (e || named == -1 || strcmp(label.name_buf, expected) || thread.tid != (int)tids[cpu] ||
            thread.real_priority > 1 || thread.last_cpu != cpu) {
            fprintf(stderr,"idle qualification cpu=%u requested_tid=%u returned_tid=%d real_priority=%u last_cpu=%u devctl=%d\n",
                    cpu,tids[cpu],thread.tid,thread.real_priority,thread.last_cpu,e);
            close(fd); errno = e ? e : EINVAL; return 0;
        }
        idle_clocks[cpu] = ClockId(1, tids[cpu]);
        if (idle_clocks[cpu] == (clockid_t)-1 || !(idle_prev[cpu] = ns_clock(idle_clocks[cpu]))) {
            close(fd); return 0;
        }
        printf("idle_cpu=%u verified_tid=%u priority=%u name=%s\n", cpu, tids[cpu], thread.real_priority,label.name_buf);
        idle_history[cpu][0] = idle_prev[cpu];
    }
    close(fd); last_cpu = now(); time_history[0] = last_cpu; return 1;
}
static void tick(uint64_t n) {
    status.watchdog_ok = watchdog_at && n - watchdog_at < PG_WATCHDOG_LIMIT;
    status.enforcer_ok = status.enforcer_seen_ns && n - status.enforcer_seen_ns < PG_HEARTBEAT_LIMIT;
    status.ready_mask = 0;
    for (unsigned role = 0; role < 3; ++role) {
        if (status.worker_seen_ns[role] && n - status.worker_seen_ns[role] < PG_HEARTBEAT_LIMIT)
            status.ready_mask |= 1u << role;
        if (status.worker_seen_ns[role] && n - status.worker_seen_ns[role] >= PG_HEARTBEAT_LIMIT)
            fault_latched = 1;
        int child_status;
        if (children[role] > 0 && waitpid(children[role], &child_status, WNOHANG) == children[role]) {
            fprintf(stderr, "worker role=%u exited; grants withdrawn\n", role);
            children[role] = 0; fault_latched = 1;
        }
    }
    /* Declared 100 ms sliding utilization window, refreshed every 10 ms. */
    ++cpu_samples;
    unsigned slot = cpu_samples % 11, base = (cpu_samples + 1) % 11;
    uint64_t delta = cpu_samples >= 10 ? n - time_history[base] : 0;
    status.cpu_valid = delta != 0;
    for (unsigned cpu = 0; cpu < 4; ++cpu) {
        uint64_t value = ns_clock(idle_clocks[cpu]);
        uint64_t previous = idle_history[cpu][base];
        if (!value || !previous || value < previous || !delta || value - previous > delta + delta / 10) {
            status.cpu_valid = 0;
        } else {
            uint64_t idle = value - previous;
            status.core_busy_pct[cpu] = idle >= delta ? 0 : (int)(100 - 100 * idle / delta);
        }
        idle_history[cpu][slot] = value;
    }
    time_history[slot] = n;
    last_cpu = n;
    status.cpu_window_end_ns = status.cpu_valid ? n : 0;
    if (n - last_sample >= UINT64_C(25000000)) {
        uint64_t elapsed = n - scenario_at;
        if (status.scenario != PG_SC_FREEZE || elapsed < UINT64_C(4000000000)) {
            int temp = 45000;
            if (status.scenario == PG_SC_RAMP) {
                uint64_t seconds = elapsed / 1000000000;
                temp += (int)(seconds > 35 ? 35000 : seconds * 1000);
            } else if (status.scenario == PG_SC_SPIKE) temp = 56000;
            else if (status.scenario == PG_SC_FREEZE) temp = 52000;
            status.temperature_milli_c = temp; status.thermal_acquired_ns = n;
        }
        last_sample = n;
    }
    status.monotonic_ns = n;
    status.sensor_age_ms = (n - status.thermal_acquired_ns) / 1000000;
    if (fault_latched) status.ready_mask = 0;
}
static int gates_allowed(uint64_t n) {
    return !fault_latched && status.ready_mask == 7 && status.watchdog_ok && status.enforcer_ok &&
           epoch && decision_id && n < grant_until;
}
static int ordered_cap(unsigned role, uint64_t n) {
    if (!gates_allowed(n)) return 0;
    if (role == 1 && !desired[0] && !desired[1] &&
        !(worker_epoch[2] == epoch && worker_id[2] == decision_id && !worker_cap[2]))
        return worker_cap[1]; /* NC2 must close first. */
    if (role == 2 && desired[1] &&
        !(worker_epoch[1] == epoch && worker_id[1] == decision_id && worker_cap[1] == desired[0]))
        return 0; /* NC1 must be restored first. */
    return desired[role - 1];
}
static void worker_report(const pg_worker_message *m, const struct _msg_info *info, int rcvid) {
    pg_worker_reply reply = {0};
    unsigned r = m->role;
    uint64_t n = now();
    if (m->version != PG_RUNTIME_VERSION || r > 2 || info->pid != children[r] ||
        m->placement.pid != children[r] || m->placement.runmask != (UINT64_C(1) << (r + 1)) ||
        m->placement.inherit_mask != (UINT64_C(1) << (r + 1)) ||
        m->placement.policy != SCHED_FIFO || m->placement.priority != (r == 0 ? 30 : r == 1 ? 10 : 8) || m->fault) {
        reply.error = EPROTO; fault_latched = 1;
    } else {
        status.placement[r + 1] = m->placement; status.worker_seen_ns[r] = n;
        worker_epoch[r] = m->epoch; worker_id[r] = m->decision_id;
        worker_cap[r] = m->applied_cap; worker_effect[r] = m->effective_ns;
        if (m->dropped > dropped[r]) {
            fprintf(stderr,"worker role=%u reports lost=%" PRIu64 "; grants withdrawn\n",r,m->dropped);
            dropped[r] = m->dropped; fault_latched = 1;
        }
        status.worker_dropped_reports = dropped[0] + dropped[1] + dropped[2];
        if (!r && m->job.id) {
            if (m->job.id > status.last_job_id) {
                jobs[(m->job.id - 1) % JOB_RING] = m->job;
                status.last_job_id = m->job.id;
                status.first_job_id = m->job.id > JOB_RING ? m->job.id - JOB_RING + 1 : 1;
                status.job_overwrites = m->job.id > JOB_RING ? m->job.id - JOB_RING : 0;
                status.critical_jobs = m->job.id;
                status.last_job_release_ns = m->job.release_ns;
                status.last_job_deadline_ns = m->job.deadline_ns;
                status.last_job_finish_ns = m->job.finish_ns;
                uint64_t response = m->job.finish_ns - m->job.release_ns;
                if (m->job.finish_ns > m->job.deadline_ns) ++status.critical_misses;
                if (response / 1000 > status.critical_max_response_us)
                    status.critical_max_response_us = response / 1000;
                uint64_t jitter = m->job.start_ns > m->job.release_ns ? m->job.start_ns - m->job.release_ns : 0;
                if (jitter / 1000 > status.critical_max_jitter_us) status.critical_max_jitter_us = jitter / 1000;
            }
            status.critical_fifo_ok = 1;
        }
        status.nc1_cap_pct = worker_cap[1]; status.nc2_cap_pct = worker_cap[2];
        status.worker_effect_ns[0] = worker_effect[1]; status.worker_effect_ns[1] = worker_effect[2];
        if (worker_epoch[1] == epoch && worker_epoch[2] == epoch &&
            worker_id[1] == decision_id && worker_id[2] == decision_id &&
            worker_cap[1] == desired[0] && worker_cap[2] == desired[1] &&
            worker_effect[1] >= issued && worker_effect[2] >= issued) {
            uint64_t effect = worker_effect[1] > worker_effect[2] ? worker_effect[1] : worker_effect[2];
            status.applied_epoch = epoch; status.applied_decision_id = decision_id;
            status.last_effective_ns = effect;
            status.last_actuation_us = (effect - issued) / 1000;
            status.level_applied = status.level_requested;
            if (desired[0] == 100 && desired[1] == 100 && clear_at) {
                status.recovery_to_full_ms = (effect - clear_at) / 1000000; clear_at = 0;
            }
        }
        reply.epoch = epoch; reply.decision_id = decision_id; reply.until_ns = grant_until;
        reply.cap = r ? ordered_cap(r, n) : 100;
        reply.demand_pct = r == 1 ? 20 : status.scenario == PG_SC_SPIKE ? 90 : 15;
        reply.stop = !running;
    }
    MsgReply(rcvid, EOK, &reply, sizeof(reply));
}
static int caps(const pg_cap_request_t *m, uint64_t n) {
    int a = m->request.arg, b = m->request.extra;
    if (a < 0 || a > 100 || b < 0 || b > 100 || (b && !a)) return EINVAL;
    if (!m->epoch || !m->decision_id || m->until_ns <= n || m->until_ns - n > UINT64_C(50000000) ||
        m->epoch < epoch || (m->epoch == epoch && m->decision_id < decision_id)) return ESTALE;
    if (m->epoch == epoch && m->decision_id == decision_id) {
        if (a != desired[0] || b != desired[1] || (n >= grant_until && (a || b))) return ESTALE;
        if (m->until_ns > grant_until) grant_until = m->until_ns;
        return 0;
    }
    if (!desired[0] && !desired[1] && (a || b)) clear_at = n;
    desired[0] = a; desired[1] = b;
    epoch = m->epoch; decision_id = m->decision_id; grant_until = m->until_ns; issued = n;
    status.level_requested = !a ? 2 : !b ? 1 : 0;
    status.applied_decision_id = 0; status.last_effective_ns = 0;
    return 0;
}
static int serve(const char *executable, char **idle_args, unsigned iterations) {
    if (pg_thread_cpu_count() != 4) { fputs("Runtime requires four qualified CPUs\n", stderr); return 1; }
    if (pg_thread_configure(0, 22, "pg-coordinator", &status.placement[0]) || !idle_setup(idle_args)) {
        perror("coordinator affinity/idle-clock qualification"); return 1;
    }
    pg_thread_print(&status.placement[0]);
    name_attach_t *attach = name_attach(NULL, PG_BENCH_NAME, 0);
    if (!attach) { perror("runtime name_attach (stop legacy pgbench first)"); return 1; }
    int coid = ConnectAttach(0, 0, attach->chid, _NTO_SIDE_CHANNEL, 0);
    timer_t timer;
    struct sigevent event;
    SIGEV_PULSE_INIT(&event, coid, 22, _PULSE_CODE_MINAVAIL, 0);
    struct itimerspec tolerance = {.it_value = {.tv_nsec = 1}};
    struct itimerspec interval = {.it_value = {.tv_nsec = TICK_NS}, .it_interval = {.tv_nsec = TICK_NS}};
    if (coid == -1 || timer_create(CLOCK_MONOTONIC, &event, &timer) ||
        timer_settime(timer, TIMER_TOLERANCE, &tolerance, NULL) || timer_settime(timer, 0, &interval, NULL)) {
        perror("runtime high resolution pulse timer"); name_detach(attach, 0); return 1;
    }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    status.features = PG_FEATURE_THREADCTL | PG_FEATURE_ENFORCER; scenario_at = now();
    status.temperature_milli_c = 45000; status.thermal_acquired_ns = scenario_at;
    status.level_requested = status.level_applied = 2;
    for (unsigned r = 0; r < 3; ++r) {
        char role[8], work[32]; snprintf(role, sizeof(role), "%u", r); snprintf(work, sizeof(work), "%u", iterations);
        char *args[] = {(char *)executable, "worker", role, work, NULL};
        int e = posix_spawn(&children[r], executable, NULL, NULL, args, environ);
        if (e) { errno = e; perror("spawn worker"); running = 0; break; }
    }
    while (running) {
        union { struct _pulse pulse; pg_request_t request; pg_cap_request_t caps;
                pg_worker_message worker; pg_jobs_request jobs; pg_enforcer_request enforcer; } in = {0};
        struct _msg_info info;
        int rcvid = MsgReceive(attach->chid, &in, sizeof(in), &info);
        if (rcvid == -1) { if (errno == EINTR) continue; perror("runtime MsgReceive"); break; }
        if (!rcvid) {
            if (in.pulse.code == _PULSE_CODE_MINAVAIL) tick(now());
            if (in.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(in.pulse.scoid);
            continue;
        }
        if (info.msglen < (ssize_t)sizeof(in.request.type)) { MsgError(rcvid, EINVAL); continue; }
        if (in.request.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
        if (in.request.type == PG_RUNTIME_TYPE) {
            if (info.srcmsglen != sizeof(in.worker) || info.msglen != sizeof(in.worker)) MsgError(rcvid, EINVAL);
            else worker_report(&in.worker, &info, rcvid);
            continue;
        }
        size_t expected = in.request.command == PG_CMD_LEASED_CAPS ? sizeof(in.caps) :
                          in.request.command == PG_CMD_ENFORCER ? sizeof(in.enforcer) :
                          in.request.command == PG_CMD_JOBS ? sizeof(in.jobs) : sizeof(in.request);
        if (info.srcmsglen != (ssize_t)expected || info.msglen != (ssize_t)expected ||
            in.request.type != PG_BENCH_TYPE || in.request.version != PG_BENCH_VERSION) {
            MsgError(rcvid, EPROTO); continue;
        }
        uint64_t n = now(); int e = 0;
        if (in.request.command == PG_CMD_JOBS) {
            pg_jobs_reply reply = {.overwritten = status.job_overwrites};
            uint64_t first = in.jobs.after_id + 1;
            if (first < status.first_job_id) reply.error = EOVERFLOW;
            else for (uint64_t id = first; id <= status.last_job_id && reply.count < PG_JOB_BATCH; ++id) {
                pg_job_record job = jobs[(id - 1) % JOB_RING];
                if (job.id != id) { reply.error = EOVERFLOW; break; }
                reply.jobs[reply.count++] = job;
            }
            MsgReply(rcvid, EOK, &reply, sizeof(reply)); continue;
        }
        switch (in.request.command) {
        case PG_CMD_STATUS: break;
        case PG_CMD_WATCHDOG: watchdog_at = n; break;
        case PG_CMD_ENFORCER: {
            const pg_thread_report *r = &in.enforcer.placement;
            if (r->pid != info.pid || r->runmask != 1 || r->inherit_mask != 1 ||
                r->cpu != 0 || r->policy != SCHED_FIFO || r->priority != 24 ||
                strcmp(r->name, "pg-enforcer")) e = EINVAL;
            else if (enforcer_pid && enforcer_pid != info.pid && status.enforcer_ok) e = EBUSY;
            else {
                enforcer_pid = info.pid; status.enforcer_seen_ns = n;
                status.enforcer_ok = 1; status.placement[4] = *r;
            }
            break;
        }
        case PG_CMD_LEASED_CAPS:
            e = info.pid != enforcer_pid ? EPERM : caps(&in.caps, n); break;
        case PG_CMD_SCENARIO:
            if (in.request.arg < PG_SC_NORMAL || in.request.arg > PG_SC_FREEZE) e = EINVAL;
            else { status.scenario = in.request.arg; scenario_at = n; last_sample = 0; }
            break;
        case PG_CMD_STATE:
            if (in.request.arg < PG_ST_UNKNOWN || in.request.arg > PG_ST_RECOVERY) e = EINVAL;
            else { status.supervisor_state = in.request.arg; status.recovery_percent = in.request.extra; }
            break;
        case PG_CMD_STOP: running = 0; break;
        default: e = ENOTSUP; break;
        }
        status.error = e; status.monotonic_ns = n;
        status.sensor_age_ms = (n - status.thermal_acquired_ns) / 1000000;
        MsgReply(rcvid, EOK, &status, sizeof(status));
    }
    timer_delete(timer); ConnectDetach(coid); name_detach(attach, 0);
    for (unsigned r = 0; r < 3; ++r) if (children[r] > 0) kill(children[r], SIGTERM);
    for (unsigned r = 0; r < 3; ++r) if (children[r] > 0) waitpid(children[r], NULL, 0);
    return fault_latched ? 1 : 0;
}
static int client(const char *command) {
    int coid = name_open(PG_BENCH_NAME, 0);
    if (coid == -1) { perror("runtime name_open"); return 1; }
    pg_status_t s;
    pg_request_t request = {PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_STATUS, 0, 0};
    if (!strcmp(command, "watchdog")) {
        pg_thread_report report;
        if (pg_thread_configure(0, 26, "pg-watchdog", &report)) { perror("watchdog qualification"); return 1; }
        pg_thread_print(&report);
        signal(SIGINT, stop); signal(SIGTERM, stop);
        unsigned failures = 0;
        while (running) {
            request.command = PG_CMD_WATCHDOG;
            if (!send_timed(coid, &request, sizeof(request), &s, sizeof(s)) || s.error) {
                if (!failures++) fputs("WATCHDOG: runtime unresponsive; worker grants expire independently\n", stderr);
            } else if (s.ready_mask != 7 || s.worker_dropped_reports) {
                if (!failures++) fputs("WATCHDOG: incomplete readiness or dropped worker reports\n", stderr);
            } else failures = 0;
            struct timespec pause = {.tv_nsec = 20000000}; nanosleep(&pause, NULL);
        }
    } else if (!strcmp(command, "jobs") || !strcmp(command, "record")) {
        int stream = !strcmp(command, "record");
        signal(SIGINT, stop); signal(SIGTERM, stop);
        pg_jobs_request q = {.request = {PG_BENCH_TYPE, PG_BENCH_VERSION, PG_CMD_JOBS, 0, 0}};
        puts("id,release_ns,deadline_ns,finish_ns,start_ns");
        while (running) {
            pg_jobs_reply reply;
            if (!send_timed(coid, &q, sizeof(q), &reply, sizeof(reply)) || reply.error) {
                fputs("job dump incomplete/overwritten\n", stderr); return 1;
            }
            for (unsigned j = 0; j < reply.count; ++j) {
                pg_job_record *job = &reply.jobs[j];
                printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                       job->id, job->release_ns, job->deadline_ns, job->finish_ns, job->start_ns);
                q.after_id = job->id;
            }
            if (reply.count < PG_JOB_BATCH) {
                if (!stream) break;
                struct timespec pause = {.tv_nsec = 20000000}; nanosleep(&pause, NULL);
            }
        }
    } else if (!strcmp(command, "samples")) {
        signal(SIGINT, stop); signal(SIGTERM, stop);
        puts("monotonic_ns,scenario,temp_sim_milli_c,sensor_age_ms,cpu0,cpu1,cpu2,cpu3,state,nc1,nc2,effect1_ns,effect2_ns,ready,watchdog,cpu_valid,misses");
        while (running) {
            if (!send_timed(coid, &request, sizeof(request), &s, sizeof(s)) || s.error) return 1;
            printf("%" PRIu64 ",%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%u,%u,%u,%" PRIu64 "\n",
                   s.monotonic_ns,s.scenario,s.temperature_milli_c,s.sensor_age_ms,
                   s.core_busy_pct[0],s.core_busy_pct[1],s.core_busy_pct[2],s.core_busy_pct[3],
                   s.supervisor_state,s.nc1_cap_pct,s.nc2_cap_pct,s.worker_effect_ns[0],s.worker_effect_ns[1],
                   s.ready_mask,s.watchdog_ok,s.cpu_valid,s.critical_misses);
            struct timespec pause = {.tv_nsec = 25000000}; nanosleep(&pause, NULL);
        }
    } else {
        if (!strcmp(command, "stop")) request.command = PG_CMD_STOP;
        if (!send_timed(coid, &request, sizeof(request), &s, sizeof(s)) || s.error) { perror("runtime status"); return 1; }
        printf("ready=0x%x watchdog=%u enforcer=%u cpu_valid=%u source=SIMULATED-temperature REAL-per-core-CPU state=%d\n",
               s.ready_mask, s.watchdog_ok, s.enforcer_ok, s.cpu_valid, s.supervisor_state);
        for (unsigned r = 0; r < 5; ++r) pg_thread_print(&s.placement[r]);
        printf("CPU=[%d,%d,%d,%d] NC1=%d NC2=%d effects=[%" PRIu64 ",%" PRIu64 "]\n",
               s.core_busy_pct[0], s.core_busy_pct[1], s.core_busy_pct[2], s.core_busy_pct[3],
               s.nc1_cap_pct, s.nc2_cap_pct, s.worker_effect_ns[0], s.worker_effect_ns[1]);
        printf("critical_jobs=%" PRIu64 " misses=%" PRIu64 " max_response_us=%" PRIu64
               " dropped_worker_reports=%" PRIu64 " overwritten_jobs=%" PRIu64 "\n",
               s.critical_jobs, s.critical_misses, s.critical_max_response_us,
               s.worker_dropped_reports, s.job_overwrites);
        int policy = name_open(PG_NAME, 0);
        if (policy == -1) puts("Supervisor unavailable: policy state and recovery not qualified");
        else {
            pg_message_v1 q = {.type = PG_SCORE, .version = PG_VERSION};
            q.body.trial.series = 1; q.body.trial.from = NORMAL; q.body.trial.to = THROTTLED;
            pg_score_reply reply;
            if (!send_timed(policy, &q, sizeof(q), &reply, sizeof(reply))) puts("Supervisor read failed");
            else {
                const pg_status *p = &reply.snapshot.decision;
                const pg_sample *in = &reply.snapshot.last_input;
                printf("temp=%.1fC source=%s quality=%d age_ms=%.3f state=%u causes=0x%x power_index=%u\n",
                       in->temp_deci_c/10.,in->source==REAL?"REAL":in->source==SIMULATED?"SIMULATED":"UNAVAILABLE",
                       in->quality, in->acquired_ns && p->as_of_ns >= in->acquired_ns ?
                       (p->as_of_ns-in->acquired_ns)/1e6:-1., p->state,p->causes,p->power_index);
                printf("requested_caps=%u/%u decision=%" PRIu64 " effective_ns=%" PRIu64
                       " recovery_clear_ms=%.3f last_critical_latency_ms=%.3f job_gaps=%" PRIu64 "\n",
                       p->nc1_pct,p->nc2_pct,p->decision_id,p->effective_ns,
                       p->clear_since_ns?(p->as_of_ns-p->clear_since_ns)/1e6:0.,
                       reply.scores.last_latency_ns/1e6,reply.scores.job_gaps);
            }
            name_close(policy);
        }
    }
    name_close(coid); return 0;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((argc == 6 || argc == 7) && !strcmp(argv[1], "serve")) {
        unsigned iterations = 100000;
        if (argc == 7 && !positive(argv[6], &iterations)) return 2;
        char path[1024];
        if (!realpath(argv[0], path)) { perror("runtime executable path"); return 1; }
        return serve(path, &argv[2], iterations);
    }
    if (argc == 4 && !strcmp(argv[1], "worker")) {
        unsigned work;
        if (strlen(argv[2]) != 1 || *argv[2] < '0' || *argv[2] > '2' || !positive(argv[3], &work)) return 2;
        return pg_worker_main((unsigned)(*argv[2] - '0'), work);
    }
    if (argc == 2 && (!strcmp(argv[1], "status") || !strcmp(argv[1], "jobs") ||
                     !strcmp(argv[1], "record") || !strcmp(argv[1], "samples") ||
                     !strcmp(argv[1], "watchdog") || !strcmp(argv[1], "stop"))) return client(argv[1]);
    fputs("pg-runtime serve IDLE_TID_CPU0 IDLE_TID_CPU1 IDLE_TID_CPU2 IDLE_TID_CPU3 [CRITICAL_ITERATIONS]\n"
          "pg-runtime status|jobs|record|samples|watchdog|stop\n", stderr);
    return 2;
}
