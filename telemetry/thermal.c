/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * telemetry/thermal.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#include "ipc.h"
#include "pg_threadctl.h"
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/trace.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sched.h>
#include <signal.h>
#include <semaphore.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <ctype.h>

#define RING PG_BATCH_MAX

enum { RELEASE = _PULSE_CODE_MINAVAIL, DATA, DONE };

typedef struct {
    pg_sample s;
    uint64_t due;
    uint64_t wake;
    uint64_t read_at;
    uint64_t read_end;
    uint64_t parsed;
    uint64_t cpu_start;
    uint64_t cpu_end;
    uint64_t published;
    uint64_t received;
    char raw[64];
    int phase;
} row;

static row *rows;
static unsigned slots[RING];
static atomic_uint head;
static atomic_uint tail;
static atomic_bool stop;
static atomic_uint_fast64_t heat_until;
static sem_t heat_start;
static int inbox;
static int notify;
static int server = -1;
static int traced;
static atomic_uint overflow;
static uint64_t receiver_cpu;
static unsigned messages;

static uint64_t ns(clockid_t id)
{
    struct timespec t;
    if (clock_gettime(id, &t)) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static uint64_t idle_time(clockid_t id)
{
    struct timespec t;
    if (clock_gettime(id, &t) < 0) {
        return UINT64_MAX;
    }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static struct timespec ns_to_timespec(uint64_t v)
{
    return (struct timespec){v / 1000000000, v % 1000000000};
}

static void die(const char *why)
{
    perror(why);
    exit(1);
}

static int connect_local(int ch)
{
    int co = ConnectAttach(0, 0, ch, _NTO_SIDE_CHANNEL, 0);
    if (co < 0) {
        die("ConnectAttach");
    }
    return co;
}

static void mark(unsigned event, unsigned seq)
{
    if (traced) {
        trace_logi(event, seq, 0);
    }
}

static int read_raw(char *out, size_t cap)
{

    int fd = open("/dev/thermal", O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    size_t used = 0;
    while (used + 1 < cap) {
        ssize_t got = read(fd, out + used, cap - 1 - used);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            close(fd);
            return 0;
        }
        if (!got) {
            break;
        }
        used += (size_t)got;
        if (memchr(out, '\n', used)) {
            break;
        }
    }
    int ok = used > 0 && used + 1 < cap && !memchr(out, 0, used);
    out[used] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return close(fd) == 0 && ok;
}

static int parse_temperature(const char *raw, int divisor, int32_t *v)
{
    char *end;
    double x = strtod(raw, &end);
    if (end == raw || !isfinite(x)) {
        return 0;
    }
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    if (*end == 'C' && divisor == 1) {
        ++end;
    }
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    x /= divisor;
    if (*end || x < -40 || x > 125) {
        return 0;
    }
    *v = (int32_t)lround(x * 10);
    return 1;
}

static void *receiver(void *unused)
{
    (void)unused;
    uint64_t begin = ns(CLOCK_THREAD_CPUTIME_ID);
    for (;;) {
        struct _pulse p;
        if (MsgReceivePulse(inbox, &p, sizeof(p), NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("receive data");
        }
        if (p.code != DATA && p.code != DONE) {
            continue;
        }
        unsigned published = atomic_load(&head);
        unsigned end = p.code == DONE ? published : (unsigned)p.value.sival_int;
        unsigned start = atomic_load(&tail);
        uint64_t received = ns(CLOCK_MONOTONIC);
        if (server >= 0 && start != end) {
            pg_sample_batch batch = {
                .type = PG_SAMPLE_BATCH, .version = PG_VERSION, .count = end - start};
            for (unsigned i = start; i != end; ++i) {
                batch.samples[i - start] = rows[slots[i % RING]].s;
            }
            uint64_t timeout = 100 * PG_MS;
            if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL,
                             &timeout, NULL) < 0 ||
                MsgSend(server, &batch, sizeof(batch), &received, sizeof(received)) < 0) {
                die("deliver samples");
            }
            ++messages;
        }
        for (unsigned i = start; i != end; ++i) {
            rows[slots[i % RING]].received = received;
        }
        atomic_store(&tail, end);
        if (p.code == DONE) {
            if (overflow && server >= 0) {
                pg_sample_batch fault = {
                    .type = PG_SAMPLE_BATCH, .version = PG_VERSION, .count = 1};
                fault.samples[0].quality = SENSOR_ERROR;
                fault.samples[0].source = REAL;
                uint64_t timeout = 100 * PG_MS;
                uint64_t ack;
                if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL,
                                 &timeout, NULL) < 0 ||
                    MsgSend(server, &fault, sizeof(fault), &ack, sizeof(ack)) < 0) {
                    die("deliver overflow fault");
                }
                ++messages;
            }
            break;
        }
    }
    receiver_cpu = ns(CLOCK_THREAD_CPUTIME_ID) - begin;
    return NULL;
}

static void *heater(void *unused)
{
    (void)unused;
    struct sched_param low = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &low)) {
        fputs("heater priority failed\n", stderr);
        exit(1);
    }
    volatile unsigned x = 1;
    while (!atomic_load(&stop)) {
        if (ns(CLOCK_MONOTONIC) < atomic_load(&heat_until)) {
            for (unsigned i = 0; i < 1024; ++i) {
                x = x * 1664525u + 1013904223u;
            }
        } else {
            while (sem_wait(&heat_start) < 0) {
                if (errno != EINTR) {
                    die("heater wait");
                }
            }
        }
    }
    return NULL;
}

static int compare_ns(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void write_distribution(FILE *f, const char *name, uint64_t *v, unsigned n)
{
    if (!n) {
        return;
    }
    qsort(v, n, sizeof(*v), compare_ns);
    fprintf(f,
            "%s_p50_ns,%" PRIu64 "\n%s_p95_ns,%" PRIu64 "\n%s_p99_ns,%" PRIu64
            "\n%s_max_ns,%" PRIu64 "\n",
            name, v[(n * 50 + 99) / 100 - 1], name, v[(n * 95 + 99) / 100 - 1], name,
            v[(n * 99 + 99) / 100 - 1], name, v[n - 1]);
}

static int parse_integer(const char *s, int min, int max)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n < min || n > max) {
        fprintf(stderr, "Invalid argument: %s\n", s);
        exit(2);
    }
    return (int)n;
}

int main(int argc, char **argv)
{
    if (argc < 9 || argc > 13) {
        fputs(
            "pg-thermal busy|wait|pulse|change C|mC seconds stop_C idle_tid0 idle_tid1 idle_tid2 idle_tid3 [trace] [live] [quiet] [40hz]\n",
            stderr);
        return 2;
    }
    pg_thread_report placement;
    if (pg_thread_configure(0, 18, "pg-thermal", &placement)) {
        perror("thermal ThreadCtl/scheduling qualification");
        return 1;
    }
    pg_thread_print(&placement);
    int wait = !strcmp(argv[1], "wait");
    int change = !strcmp(argv[1], "change");
    int busy_wait = !strcmp(argv[1], "busy");
    int quiet = 0;
    uint64_t period = 50 * PG_MS;
    if (!busy_wait && !wait && !change && strcmp(argv[1], "pulse")) {
        return 2;
    }
    int divisor = !strcmp(argv[2], "C") ? 1 : !strcmp(argv[2], "mC") ? 1000 : 0;
    if (!divisor) {
        return 2;
    }
    int seconds = parse_integer(argv[3], 3, 1800);
    int cutoff = parse_integer(argv[4], 1, 85) * 10;

    clockid_t idle[4];
    uint64_t prev[4];
    uint64_t prev_end;
    for (int i = 0; i < 4; ++i) {
        int tid = parse_integer(argv[5 + i], 1, 65535);
        for (int j = 0; j < i; ++j) {
            if (tid == atoi(argv[5 + j])) {
                return 2;
            }
        }
        idle[i] = ClockId(1, tid);
        if (idle[i] == -1) {
            die("idle ClockId");
        }
    }
    for (int i = 9; i < argc; ++i) {
        if (!strcmp(argv[i], "trace")) {
            traced = 1;
        } else if (!strcmp(argv[i], "quiet")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "40hz")) {
            period = 25 * PG_MS;
        } else if (!strcmp(argv[i], "live")) {
            server = name_open(PG_NAME, 0);
            if (server < 0) {
                die("name_open");
            }
        } else {
            return 2;
        }
    }
    unsigned count = (unsigned)(seconds * UINT64_C(1000000000) / period);
    int pulse_timer = !wait && !busy_wait;
    char probe[64] = {0};
    int32_t probe_temp;
    if (!read_raw(probe, sizeof(probe)) || !parse_temperature(probe, divisor, &probe_temp)) {
        fprintf(stderr, "Cannot qualify /dev/thermal with supplied units: '%s'\n", probe);
        return 1;
    }
    FILE *csv = fopen("samples.csv", "wx");
    FILE *sum = fopen("summary.csv", "wx");
    if (!csv || !sum) {
        die("create outputs in a fresh run directory");
    }
    rows = calloc(count, sizeof(*rows));
    uint64_t *values = calloc(count, sizeof(*values));
    if (!rows || !values) {
        die("allocate run records");
    }
    inbox = ChannelCreate(_NTO_CHF_PRIVATE);
    if (inbox < 0) {
        die("ChannelCreate data");
    }
    notify = connect_local(inbox);
    if (sem_init(&heat_start, 0, 0)) {
        die("heater semaphore");
    }
    pthread_t sink;
    pthread_t load;
    if (pthread_create(&sink, NULL, receiver, NULL) || pthread_create(&load, NULL, heater, NULL)) {
        return 1;
    }
    int rel = -1;
    timer_t timer;
    if (pulse_timer) {
        rel = ChannelCreate(_NTO_CHF_PRIVATE);
        if (rel < 0) {
            die("ChannelCreate release");
        }
        struct sigevent event;
        SIGEV_PULSE_INIT(&event, connect_local(rel), -1, RELEASE, 0);
        if (timer_create(CLOCK_MONOTONIC, &event, &timer)) {
            die("timer_create");
        }
        struct itimerspec tol = {.it_value = {0, 1}};
        if (timer_settime(timer, TIMER_TOLERANCE, &tol, NULL)) {
            die("high resolution timer ability");
        }
    }
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution)) {
        die("clock_getres");
    }
    uint64_t overhead = UINT64_MAX;
    for (int i = 0; i < 100; ++i) {
        uint64_t a = ns(CLOCK_MONOTONIC);
        uint64_t b = ns(CLOCK_MONOTONIC);
        if (b - a < overhead) {
            overhead = b - a;
        }
    }
    for (int i = 0; i < 4; ++i) {
        prev[i] = idle_time(idle[i]);
    }
    prev_end = ns(CLOCK_MONOTONIC);
    uint64_t first = prev_end + period;
    uint64_t due = first;
    uint64_t end = first + count * period;
    if (pulse_timer) {
        struct itimerspec spec = {.it_value = ns_to_timespec(first),
                                  .it_interval = ns_to_timespec(period)};
        if (timer_settime(timer, TIMER_ABSTIME, &spec, NULL)) {
            die("arm release");
        }
    }
    unsigned n = 0;
    unsigned wakes = 0;
    unsigned signals = 0;
    unsigned lost = 0;
    unsigned errors = 0;
    int last_key[6] = {-2};
    int heat_blocked = 0;
    unsigned bad_cpu = 0;
    uint64_t heartbeat = first;
    uint64_t last_good = 0;
    uint64_t seq = prev_end;
    uint64_t begin_cpu = ns(CLOCK_THREAD_CPUTIME_ID);
    int32_t last_temp = 0;
    while (due < end) {
        if (busy_wait) {
            while (ns(CLOCK_MONOTONIC) < due) {
            }
        } else if (wait) {
            uint64_t tolerance = 1;
            if (TimerTimeout(CLOCK_MONOTONIC, TIMER_TOLERANCE, NULL, &tolerance, NULL) < 0) {
                die("wait tolerance");
            }
            if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_NANOSLEEP | TIMER_ABSTIME, NULL, &due,
                             NULL) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                die("timed wait");
            }
        } else {
            struct _pulse p;
            if (MsgReceivePulse(rel, &p, sizeof(p), NULL) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                die("release");
            }
            ++wakes;
        }
        uint64_t now = ns(CLOCK_MONOTONIC);
        if (now < due) {
            continue;
        }
        if (now >= end) {
            break;
        }
        due += ((now - due) / period) * period;
        row *r = &rows[n];
        r->due = due;
        r->wake = now;
        r->phase = quiet ? 0 : (int)((due - first) * 3 / (end - first));
        mark(81, n);
        r->read_at = ns(CLOCK_MONOTONIC);
        int ok = read_raw(r->raw, sizeof(r->raw));
        r->read_end = ns(CLOCK_MONOTONIC);
        ok = ok && parse_temperature(r->raw, divisor, &last_temp);
        r->parsed = ns(CLOCK_MONOTONIC);
        mark(82, n);
        if (ok) {
            last_good = r->read_at;
            ++seq;
        } else {
            ++errors;
        }
        r->s = (pg_sample){.thermal_seq = seq,
                           .acquired_ns = last_good,
                           .temp_deci_c = last_temp,
                           .quality = ok ? READ_ONLY : SENSOR_ERROR,
                           .source = REAL};
        if (!ok || last_temp >= cutoff) {
            heat_blocked = 1;
        }
        int heating = r->phase == 1 && !heat_blocked;
        uint64_t permit = heating ? r->read_at + 150 * PG_MS : 0;
        uint64_t phase_end = first + (2 * count + 2) / 3 * period;
        if (permit > phase_end) {
            permit = phase_end;
        }
        uint64_t previous = atomic_exchange(&heat_until, permit);
        if (permit && previous <= r->parsed && sem_post(&heat_start)) {
            die("heater signal");
        }
        r->s.nc_demand_pct[1] = heating ? 100 : 0;
        mark(83, n);
        r->cpu_start = ns(CLOCK_MONOTONIC);
        uint64_t cur[4];
        for (int i = 0; i < 4; ++i) {
            cur[i] = idle_time(idle[i]);
        }
        r->cpu_end = ns(CLOCK_MONOTONIC);
        mark(84, n);
        int peak = 0;
        for (int i = 0; i < 4; ++i) {
            double busy = 100.0 * (1.0 - (double)(cur[i] - prev[i]) / (r->cpu_end - prev_end));
            int u = prev[i] == UINT64_MAX || cur[i] == UINT64_MAX || cur[i] < prev[i] || busy < -2
                        ? -1
                    : busy < 0 ? 0
                               : (int)lround(busy);
            r->s.core_busy_pct[i] = u;
            prev[i] = cur[i];
            if (u > peak) {
                peak = u;
            }
        }
        prev_end = r->s.load_end_ns = r->cpu_end;
        int index = heating ? 60 : 0;
        unsigned invalid_mask = 0;
        for (int i = 0; i < 4; ++i) {
            if (r->s.core_busy_pct[i] < 0) {
                invalid_mask |= 1u << i;
            }
        }
        if (invalid_mask) {
            ++bad_cpu;
        }
        int key[] = {pg_activation_limit(0, last_temp),
                     pg_activation_limit(1, peak),
                     pg_activation_limit(2, index),
                     pg_clear_values(last_temp, peak, index),
                     ok,
                     (int)invalid_mask};
        unsigned h = atomic_load(&head);
        r->published = ns(CLOCK_MONOTONIC);
        if (h - atomic_load(&tail) >= RING) {
            ++lost;
            atomic_store(&overflow, 1);
            atomic_store(&heat_until, 0);
            break;
        }
        slots[h % RING] = n++;
        atomic_store(&head, h + 1);
        if (!change || memcmp(key, last_key, sizeof(key)) != 0 || due >= heartbeat) {
            if (MsgSendPulse(notify, -1, DATA, (int)(h + 1)) < 0) {
                die("data pulse");
            }
            ++signals;
            heartbeat = due + 100 * PG_MS;
        }
        memcpy(last_key, key, sizeof(key));
        due += period;
    }
    uint64_t cpu = ns(CLOCK_THREAD_CPUTIME_ID) - begin_cpu;
    atomic_store(&stop, true);
    atomic_store(&heat_until, 0);
    if (sem_post(&heat_start)) {
        die("heater stop");
    }
    if (pulse_timer) {
        timer_delete(timer);
    }
    if (MsgSendPulse(notify, -1, DONE, 0) < 0) {
        die("final drain");
    }
    pthread_join(sink, NULL);
    pthread_join(load, NULL);
    fprintf(
        csv,
        "phase,due_ns,wake_ns,raw,temp_C,quality,seq,acquired_ns,read_start_ns,read_end_ns,parsed_ns,cpu_start_ns,load_end_ns,cpu0,cpu1,cpu2,cpu3,peak,mean,demand_index,published_ns,received_ns\n");
    int phase_min[3] = {1250, 1250, 1250};
    int phase_max[3] = {-400, -400, -400};
    unsigned valid[3] = {0};
    for (unsigned i = 0; i < n; ++i) {
        row *r = &rows[i];
        int peak = 0;
        int total = 0;
        if (r->s.quality == READ_ONLY) {
            int phase = r->phase;
            int t = r->s.temp_deci_c;
            if (t < phase_min[phase]) {
                phase_min[phase] = t;
            }
            if (t > phase_max[phase]) {
                phase_max[phase] = t;
            }
            ++valid[phase];
        }
        for (int j = 0; j < 4; ++j) {
            int u = r->s.core_busy_pct[j];
            if (u < 0) {
                peak = total = -1;
                break;
            }
            total += u;
            if (u > peak) {
                peak = u;
            }
        }
        fprintf(csv, "%s,%" PRIu64 ",%" PRIu64 ",\"",
                (const char *[]){"idle", "load", "cooldown"}[r->phase], r->due, r->wake);
        for (const char *p = r->raw; *p; ++p) {
            if (*p == '"') {
                fputc('"', csv);
            }
            fputc(*p, csv);
        }
        fprintf(csv,
                "\",%.1f,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%d,%d,%d,%d,%d,%.2f,%d,%" PRIu64 ",%" PRIu64 "\n",
                r->s.temp_deci_c / 10., r->s.quality == READ_ONLY ? "read_ok" : "error",
                r->s.thermal_seq, r->s.acquired_ns, r->read_at, r->read_end, r->parsed,
                r->cpu_start, r->cpu_end, r->s.core_busy_pct[0], r->s.core_busy_pct[1],
                r->s.core_busy_pct[2], r->s.core_busy_pct[3], peak, total < 0 ? -1. : total / 4.,
                r->s.nc_demand_pct[1] * 3 / 5, r->published, r->received);
    }
    fprintf(
        sum,
        "metric,value\nsource,HARDWARE\nmode,%s\nunits,%s\nsink,%s\nplanned,%u\ncollected,%u\nmissing,%u\noverflow,%u\nread_errors,%u\nheat_cutoff_latched,%d\ntimer_pulses,%u\ndata_pulses,%u\npolicy_messages,%u\nsampler_cpu_ns,%" PRIu64
        "\nreceiver_cpu_ns,%" PRIu64 "\nclock_resolution_ns,%ld\nclock_pair_min_ns,%" PRIu64 "\n",
        argv[1], argv[2], server >= 0 ? "supervisor" : "benchmark", count, n, count - n, lost,
        errors, heat_blocked, wakes, signals, messages, cpu, receiver_cpu, resolution.tv_nsec,
        overhead);
    fprintf(sum,
            "period_ns,%" PRIu64 "\nduration_s,%d\nquiet,%d\nstop_C,%d\ninvalid_cpu_windows,%u\n",
            period, seconds, quiet, cutoff / 10, bad_cpu);
    for (int k = 0; k < 6; ++k) {
        unsigned used = 0;
        for (unsigned i = 0; i < n; ++i) {
            row *r = &rows[i];
            uint64_t d[] = {r->wake - r->due,           r->read_end - r->read_at,
                            r->parsed - r->read_end,    r->cpu_end - r->cpu_start,
                            r->received - r->published, r->received - r->s.acquired_ns};
            if (k != 5 || r->s.acquired_ns) {
                values[used++] = d[k];
            }
        }
        write_distribution(sum,
                           (const char *[]){"lateness", "read", "parse", "cpu_collection",
                                            "delivery", "read_age"}[k],
                           values, used);
    }
    for (int i = 0; i < 3; ++i) {
        const char *phase = (const char *[]){"idle", "load", "cooldown"}[i];
        if (valid[i]) {
            fprintf(sum, "%s_min_C,%.1f\n%s_max_C,%.1f\n", phase, phase_min[i] / 10., phase,
                    phase_max[i] / 10.);
        }
    }
    int failed = ferror(csv) || ferror(sum);
    if (fclose(csv)) {
        failed = 1;
    }
    if (fclose(sum)) {
        failed = 1;
    }
    return failed || lost || bad_cpu || errors || n != count;
}
