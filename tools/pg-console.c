/* A portable interactive policy bench and a native QNX IPC test client. */
#include "ipc.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __QNXNTO__
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>
#endif

static const char *states[] = {"Normal","Warning","Throttled","Critical","Recovery"};
typedef struct {
    pg_policy policy;
    pg_metrics metrics;
    pg_reply_v1 snap;
    pg_sample sample;
    uint64_t now;
    unsigned series, from, to;
    int live, coid;
} bench;

static double ms(uint64_t ns) { return (double)ns / PG_MS; }

static int integer(const char *s, long lo, long hi, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s,&end,10);
    if (errno || s == end || *end || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

static int state_id(const char *s) {
    for (int i=0; i<5; ++i) if (!strcmp(s,states[i])) return i;
    return -1;
}

#ifdef __QNXNTO__
static uint64_t target_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static int send_qnx(bench *b, pg_message_v1 *q, void *reply, size_t size) {
    uint64_t timeout = 100 * PG_MS;
    if (TimerTimeout(CLOCK_MONOTONIC,_NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL,&timeout,NULL) == -1 ||
        MsgSend(b->coid,q,sizeof(*q),reply,size) == -1) {
        perror("PowerGuard IPC"); return 0;
    }
    return 1;
}
#endif

static void snapshot(bench *b) {
    b->snap.decision = b->policy.status;
    b->snap.last_input = b->policy.sample;
}

static void display(const bench *b, const pg_status *before) {
    const pg_status *s = &b->snap.decision;
    const pg_sample *in = &b->snap.last_input;
    printf("[%s +%.0f ms] ", b->live ? "QNX" : "SIM", ms(s->as_of_ns - s->epoch));
    if (before->state != s->state)
        printf("%s -> ", states[before->state]);
    printf("%s  temp=%.1f C (%s) age=",states[s->state],in->temp_deci_c/10.0,
           in->source == REAL ? "REAL" : in->source == SIMULATED ? "SIMULATED" : "unavailable");
    if (in->acquired_ns && in->acquired_ns <= s->as_of_ns)
        printf("%.1f ms",ms(s->as_of_ns-in->acquired_ns));
    else printf("unknown");
    printf("  CPU=%u%%  power=%u  NC1=%u%% NC2=%u%%  decision=%llu %s",
           s->cpu_peak_pct,s->power_index,s->nc1_pct,s->nc2_pct,
           (unsigned long long)s->decision_id,s->effective_ns ? "effective" : "awaiting effect");
    if (s->causes & TEMP) printf(" heat");
    if (s->causes & CPU) printf(" CPU-load");
    if (s->causes & POWER) printf(" power-demand");
    if (s->causes & BAD_TEMP) printf(" invalid-temperature");
    if (s->causes & BAD_LOAD) printf(" invalid-load");
    if (s->causes & WAIT_EFFECT) printf(" manager-timeout");
    if (s->clear_since_ns) printf("  clear for %.0f ms",ms(s->as_of_ns-s->clear_since_ns));
    puts("");
}

/* Both workers request the same duty, so the derived P equals the value typed. */
static int frame(bench *b, int temp, const int cores[4], int demand, int quality) {
    uint64_t t = b->now + PG_TICK;
#ifdef __QNXNTO__
    if (b->live) t = target_now();
#endif
    pg_sample *s = &b->sample;
    if (quality >= 0) {
        s->thermal_seq = t;
        s->acquired_ns = t;
        s->temp_deci_c = temp;
        s->quality = quality;
        s->source = SIMULATED;
    } /* In stale mode the original acquisition is repeated verbatim. */
    s->load_end_ns = t;
    for (int i=0; i<4; ++i) s->core_busy_pct[i] = cores[i];
    s->nc_demand_pct[0] = s->nc_demand_pct[1] = demand;
    b->now = t;
#ifdef __QNXNTO__
    if (b->live) {
        pg_message_v1 q = {.version=PG_VERSION,.type=PG_SAMPLE};
        q.body.sample = *s;
        return send_qnx(b,&q,&b->snap,sizeof(b->snap));
    }
#endif
    pg_status old = b->policy.status;
    pg_ingest(&b->policy,s,t);
    pg_observe(&b->metrics,&old,&b->policy);
    old = b->policy.status;
    /* Only the local bench models a 1 ms actuator. */
    uint64_t effect = old.effective_ns ? old.effective_ns : t + PG_MS;
    if (!pg_effect(&b->policy,old.epoch,old.decision_id,effect,old.nc1_pct,old.nc2_pct,t+PG_MS)) return 0;
    pg_tick(&b->policy,t+PG_MS);
    pg_observe(&b->metrics,&old,&b->policy);
    snapshot(b);
    return 1;
}

static int readings(bench *b,int argc,char **v) {
    if (argc < 3 || argc > 5) return 0;
    char *end;
    double temp = strtod(v[0],&end);
    if (v[0] == end || *end || !isfinite(temp) || temp < -40 || temp > 125) return 0;
    int cores[4] = {0};
    char *part = v[1];
    for (int i=0;;++i) {
        char *comma = strchr(part,',');
        if (comma) *comma = 0;
        long value;
        if (!integer(part,0,100,&value)) return 0;
        cores[i] = (int)value;
        if (!comma) { if (i != 0 && i != 3) return 0; break; }
        if (i >= 3) return 0;
        part = comma+1;
    }
    long power, duration = 10;
    if (!integer(v[2],0,100,&power)) return 0;
    int quality = GOOD;
    if (argc >= 4) {
        if (!strcmp(v[3],"stale")) quality = -1;
        else if (!strcmp(v[3],"frozen")) quality = FROZEN;
        else if (!strcmp(v[3],"missing")) quality = UNAVAILABLE;
        else if (strcmp(v[3],"good")) return 0;
    }
    if (argc == 5 && (!integer(v[4],10,600000,&duration) || duration%10)) return 0;
    for (long i=0; i<duration; i+=10) {
        pg_status before = b->snap.decision;
        if (!frame(b,(int)lround(temp*10),cores,(int)power,quality)) return -1;
        pg_status *s = &b->snap.decision;
        if (i == 0 || i+10 == duration || s->state != before.state ||
            s->decision_id != before.decision_id || s->causes != before.causes ||
            s->effective_ns != before.effective_ns || s->clear_since_ns != before.clear_since_ns) {
            display(b,&before);
            fflush(stdout); /* Show the line immediately in the Momentics Console. */
        }
#ifdef __QNXNTO__
        if (b->live && i+10 < duration) {
            struct timespec delay = {.tv_nsec=PG_TICK};
            while (nanosleep(&delay,&delay) < 0 && errno == EINTR) {}
        }
#endif
    }
    return 1;
}

static int report(bench *b) {
    pg_scores scores = b->metrics.scores;
    pg_spread jitter = b->metrics.trials[b->series-1][b->from][b->to];
#ifdef __QNXNTO__
    if (b->live) {
        pg_message_v1 q = {.version=PG_VERSION,.type=PG_SCORE};
        q.body.trial.series=b->series; q.body.trial.from=b->from; q.body.trial.to=b->to;
        pg_score_reply reply;
        if (!send_qnx(b,&q,&reply,sizeof(reply))) return -1;
        scores=reply.scores; jitter=reply.jitter; b->snap=reply.snapshot;
    }
#endif
    printf("%s scorecard\n",b->live ? "QNX supervisor" : "SIMULATED bench");
    printf("1. Restriction within %.1f ms: ",ms(PG_RESPONSE_TARGET));
    const pg_status *current = &b->snap.decision;
    int overdue = scores.pending_id && current->as_of_ns - current->issued_ns > PG_RESPONSE_TARGET;
    if (scores.late || scores.unconfirmed || overdue) printf("FAIL");
    else if (scores.pending_id) printf("pending");
    else if (!scores.completed) printf("no restriction measured");
    else printf("PASS");
    printf("  confirmed=%llu late=%llu unconfirmed=%llu pending=%u",
        (unsigned long long)scores.completed,(unsigned long long)scores.late,
        (unsigned long long)scores.unconfirmed,scores.pending_id != 0);
    if (scores.completed) printf(" worst=%.3f ms",ms(scores.worst_response_ns));
    puts("");
    if (scores.jobs) {
        printf("2. Critical deadline misses: %llu/%llu reported jobs, missing records=%llu",
            (unsigned long long)scores.misses,(unsigned long long)scores.jobs,
            (unsigned long long)scores.job_gaps);
        printf(", max completed latency=%.3f ms",ms(scores.max_latency_ns));
        if (scores.last_finished) printf(", latest latency=%.3f ms",ms(scores.last_latency_ns));
        puts("");
    } else puts("2. Critical deadline misses: no job records");
    printf("3. Worst observed overshoot in 100 ms windows:");
    if (scores.observed_mask & 1) printf(" thermal=%.1f C",scores.overshoot[0]/10.0);
    if (scores.observed_mask & 2) printf(" CPU=%d percentage points",scores.overshoot[1]);
    if (scores.observed_mask & 4) printf(" power=%d index points",scores.overshoot[2]);
    if (!scores.observed_mask) printf(" no threshold samples");
    puts("");
    printf("4. Transition jitter, case %u %s to %s: ",b->series,states[b->from],states[b->to]);
    if (jitter.count >= 2) printf("%.3f ms",ms(jitter.max_ns-jitter.min_ns));
    else printf("need two matching trials");
    printf(" (n=%llu)\n",(unsigned long long)jitter.count);
    printf("5. Controlled restoration: ");
    if (scores.restores) printf("last=%.3f ms worst=%.3f ms n=%llu\n",
        ms(scores.last_restore_ns),ms(scores.max_restore_ns),(unsigned long long)scores.restores);
    else puts("no completed recovery");
    return 1;
}

static int command(bench *b,int argc,char **v) {
    if (!argc) return 1;
    if (!strcmp(v[0],"scores")) {
        if (argc == 4) {
            long series;
            int from=state_id(v[2]), to=state_id(v[3]);
            if (!integer(v[1],1,PG_CASES,&series) || from<0 || to<0 || from==to) return 0;
            b->series=(unsigned)series; b->from=(unsigned)from; b->to=(unsigned)to;
        } else if (argc != 1) return 0;
        return report(b);
    }
    if (!strcmp(v[0],"trial") && argc == 3) {
        long series;
        int target = state_id(v[2]);
        if (!integer(v[1],1,PG_CASES,&series) || target < 0) return 0;
        unsigned from = b->snap.decision.state;
#ifdef __QNXNTO__
        if (b->live) {
            pg_message_v1 q = {.version=PG_VERSION,.type=PG_READ};
            if (!send_qnx(b,&q,&b->snap,sizeof(b->snap))) return -1;
            from=b->snap.decision.state;
            q.type=PG_TRIAL; q.body.trial.epoch=b->snap.decision.epoch;
            q.body.trial.series=(unsigned)series; q.body.trial.from=from;
            q.body.trial.to=(unsigned)target; q.body.trial.trigger_ns=target_now();
            if (!send_qnx(b,&q,&b->snap,sizeof(b->snap))) return -1;
        } else
#endif
        if (!pg_trial(&b->metrics,(unsigned)series,from,(unsigned)target,b->now,b->now)) return 0;
        b->series=(unsigned)series; b->from=from; b->to=(unsigned)target;
        return 1;
    }
    if (!b->live && !strcmp(v[0],"job") && argc == 3) {
        long elapsed,deadline;
        if (!integer(v[1],0,600000,&elapsed) || !integer(v[2],1,600000,&deadline) ||
            (uint64_t)elapsed*PG_MS >= b->now) return 0;
        return pg_job(&b->metrics,b->metrics.scores.last_job_id+1,
            b->now-(uint64_t)elapsed*PG_MS,(uint64_t)deadline*PG_MS,b->now,b->now);
    }
    return readings(b,argc,v);
}

static int line(bench *b,char *input) {
    char *tokens[6]; int n=0;
    for (char *w=strtok(input," \t\r\n"); w; w=strtok(NULL," \t\r\n")) {
        if (n==6) return 0;
        tokens[n++]=w;
    }
    return command(b,n,tokens);
}

int main(int argc,char **argv) {
    bench b={.series=1,.from=NORMAL,.to=THROTTLED};
    int example=0, first=1;
    if (argc>1 && !strcmp(argv[1],"--example")) { example=1; first++; }
    else if (argc>1 && !strcmp(argv[1],"--live")) { b.live=1; first++; }
    if (example && first != argc) { fputs("--example takes no values\n",stderr); return 2; }
    if (b.live) {
#ifdef __QNXNTO__
        b.coid=name_open(PG_NAME,0);
        if (b.coid < 0) { perror("start PowerGuard first"); return 1; }
        pg_message_v1 q={.type=PG_READ,.version=PG_VERSION};
        if (!send_qnx(&b,&q,&b.snap,sizeof(b.snap))) return 1;
        b.sample=b.snap.last_input;
#else
        fputs("--live requires the QNX target\n",stderr); return 2;
#endif
    } else {
        puts("SIMULATED bench: synthetic time, sensor readings and actuator effects.");
        b.now=PG_MS;
        pg_init(&b.policy,b.now);
        int quiet[4]={0};
        for (int i=0;i<270;i++) if (!frame(&b,500,quiet,20,GOOD)) return 1;
    }
    if (first<argc) {
        int result=command(&b,argc-first,argv+first);
        if (!result) fputs("Expected: TEMP_C CPU_PERCENT POWER_INDEX [good|stale|frozen|missing [duration_ms]]\n",stderr);
        return result == 1 ? 0 : 2;
    }
    if (example) {
        const char *script[]={"trial 1 Throttled","55 86 30","job 7 10","55 30 30 good 2000",
            "trial 1 Throttled","55 86 30","job 12 10","55 30 30 good 2000",
            "66 20 20","71 20 20","55 20 20 frozen","55 20 20 good 3000","scores"};
        for (unsigned i=0;i<sizeof(script)/sizeof(*script);++i) {
            char buf[80]; strcpy(buf,script[i]); puts(script[i]);
            if (line(&b,buf)!=1) return 1;
        }
        return 0;
    }
    puts("Enter TEMP_C CPU_PERCENT POWER_INDEX [good|stale|frozen|missing [duration_ms]]");
    puts("Examples: 55 86 30  |  55 10,86,20,10 30 good 100  |  scores  |  quit");
    if (b.live) puts("Samples stream for the entered duration. During input silence the service checks freshness.");
    char input[256];
    while (fputs("> ",stdout), fflush(stdout), fgets(input,sizeof(input),stdin)) {
        if (!strchr(input,'\n') && !feof(stdin)) {
            int c; while ((c=getchar())!=EOF && c!='\n') {}
            puts("Input line too long. Try again."); continue;
        }
        input[strcspn(input,"\r\n")]=0;
        if (!strcmp(input,"quit")) break;
        int result=line(&b,input);
        if (!result) puts("Invalid input. Use -40..125 C, integer CPU/power 0..100 and duration 10..600000 ms, in 10 ms steps.");
        if (result<0) return 1;
    }
    return 0;
}
