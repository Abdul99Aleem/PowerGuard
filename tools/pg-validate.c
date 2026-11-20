/* Target-side assertions. Exit nonzero instead of interpreting a status dump as PASS. */
#include "ipc.h"
#include "pgbench_protocol.h"
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
static int send(int coid, const void *q, size_t size, void *r, size_t capacity) {
    uint64_t timeout = 20 * PG_MS;
    return TimerTimeout(CLOCK_MONOTONIC,_NTO_TIMEOUT_SEND|_NTO_TIMEOUT_REPLY,NULL,&timeout,NULL) != -1 &&
           MsgSend(coid,q,size,r,capacity) != -1;
}
int main(int argc, char **argv) {
    const char *names[] = {"normal","warning","throttled","critical","recovery"};
    unsigned expected = 5;
    int closed = argc == 2 && !strcmp(argv[1], "closed");
    if (argc == 2) for (unsigned i = 0; i < 5; ++i) if (!strcmp(argv[1],names[i])) expected = i;
    if (expected == 5 && !closed) { fputs("pg-validate normal|warning|throttled|critical|recovery|closed\n",stderr); return 2; }
    int runtime = name_open(PG_BENCH_NAME,0), policy = name_open(PG_NAME,0);
    if (runtime == -1 || policy == -1) { perror("validation connect"); return 1; }
    pg_request_t rq = {PG_BENCH_TYPE,PG_BENCH_VERSION,PG_CMD_STATUS,0,0};
    pg_status_t s;
    pg_message_v1 pq = {.type=PG_SCORE,.version=PG_VERSION};
    pq.body.trial.series=1; pq.body.trial.from=NORMAL; pq.body.trial.to=WARNING;
    pg_score_reply p;
    if (!send(runtime,&rq,sizeof(rq),&s,sizeof(s)) || !send(policy,&pq,sizeof(pq),&p,sizeof(p))) return 1;
    name_close(runtime); name_close(policy);
    int ok = !s.error && (s.features & PG_FEATURE_THREADCTL) && s.ready_mask==7 &&
        s.enforcer_ok &&
        (closed || s.watchdog_ok) && s.cpu_valid && s.critical_jobs && !s.critical_misses && !s.worker_dropped_reports &&
        !p.scores.misses && !p.scores.job_gaps && (closed || p.snapshot.decision.state==expected);
    for (unsigned role=0;role<4;++role) {
        const pg_thread_report *r=&s.placement[role];
        int priority=role==0?22:role==1?30:role==2?10:8;
        if (r->runmask!=(UINT64_C(1)<<role) || r->inherit_mask!=r->runmask ||
            r->cpu!=(int)role || r->policy!=SCHED_FIFO || r->priority!=priority || !r->pid || !r->tid)
            ok=0;
        for (unsigned earlier=0;earlier<role;++earlier)
            if (r->pid==s.placement[earlier].pid) ok=0;
    }
    const pg_thread_report *enforcer=&s.placement[4];
    if (enforcer->runmask!=1 || enforcer->inherit_mask!=1 || enforcer->cpu!=0 ||
        enforcer->policy!=SCHED_FIFO || enforcer->priority!=24 || !enforcer->pid || !enforcer->tid) ok=0;
    for (unsigned role=0;role<4;++role) if (enforcer->pid==s.placement[role].pid) ok=0;
    const pg_status *d=&p.snapshot.decision;
    if (!closed && (s.nc1_cap_pct!=(int)d->nc1_pct || s.nc2_cap_pct!=(int)d->nc2_pct ||
        s.applied_epoch!=d->epoch || s.applied_decision_id!=d->decision_id || !d->effective_ns)) ok=0;
    if (closed && (s.nc1_cap_pct || s.nc2_cap_pct)) ok=0;
    if (expected==NORMAL && (s.nc1_cap_pct!=100 || s.nc2_cap_pct!=100)) ok=0;
    if (expected==THROTTLED && (s.nc1_cap_pct!=100 || s.nc2_cap_pct!=0)) ok=0;
    if (expected==CRITICAL && (s.nc1_cap_pct!=0 || s.nc2_cap_pct!=0 ||
                             s.worker_effect_ns[1]>s.worker_effect_ns[0])) ok=0;
    printf("%s expected=%s observed=%u ready=%x watchdog=%u CPUvalid=%u caps=%d/%d jobs=%" PRIu64
           " misses=%" PRIu64 " gaps=%" PRIu64 " dropped=%" PRIu64 "\n",
           ok?"PASS":"FAIL",closed?"closed":names[expected],d->state,s.ready_mask,s.watchdog_ok,s.cpu_valid,
           s.nc1_cap_pct,s.nc2_cap_pct,s.critical_jobs,s.critical_misses,p.scores.job_gaps,s.worker_dropped_reports);
    return ok?0:1;
}
