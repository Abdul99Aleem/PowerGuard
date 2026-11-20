/* B5 runs last: a watchdog trip intentionally requires a complete new run. */
#include "ipc.h"
#include "pgbench_protocol.h"
#include "pg_progress.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t watched;
static void interrupted(int sig) { if (watched>0) kill(watched,SIGCONT); _exit(128+sig); }
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) interrupted(1);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+t.tv_nsec;
}
static void pause_ms(unsigned ms) {
    struct timespec t={.tv_nsec=ms*PG_MS};
    while(nanosleep(&t,&t)==-1 && errno==EINTR) {}
}
static int send(int co,const void *q,size_t n,void *r,size_t size) {
    uint64_t timeout=5*PG_MS;
    return TimerTimeout(CLOCK_MONOTONIC,_NTO_TIMEOUT_SEND|_NTO_TIMEOUT_REPLY,NULL,&timeout,NULL)!=-1 &&
           MsgSend(co,q,n,r,size)!=-1;
}
int main(int argc,char **argv) {
    char *end=NULL;
    long pid=argc==2?strtol(argv[1],&end,10):0;
    if(argc!=2 || !*argv[1] || !end || *end || pid<=1 || pid>INT32_MAX) return 2;
    pg_thread_report placement;
    if(pg_thread_configure(0,18,"pg-b5-test",&placement)) return 1;
    pg_progress *progress=pg_progress_open(0);
    int runtime=name_open(PG_BENCH_NAME,0),policy=name_open(PG_NAME,0);
    if(!progress || runtime<0 || policy<0) {perror("B5 connect");return 1;}
    pg_message_v1 q={.type=PG_READ,.version=PG_VERSION}; pg_reply_v1 p;
    pg_request_t rq={PG_BENCH_TYPE,PG_BENCH_VERSION,PG_CMD_STATUS,0,0}; pg_status_t s;
    if(!send(policy,&q,sizeof q,&p,sizeof p) || p.decision.state!=NORMAL || pg_progress_forced(progress)) return 1;
    watched=(sig_atomic_t)pid;
    signal(SIGINT,interrupted);signal(SIGTERM,interrupted);
    uint64_t stopped=now();
    if(kill(watched,SIGSTOP)) {watched=0;return 1;}
    uint64_t observed=0,effective=0;
    while(now()-stopped<PG_PROGRESS_LIMIT+100*PG_MS) {
        if(!send(runtime,&rq,sizeof rq,&s,sizeof s)) break;
        if(s.forced_off && !s.nc1_cap_pct && !s.nc2_cap_pct) {
            observed=now();
            /* The legacy bench publishes the aggregate ACK. Split runtime
             * additionally exposes the two worker timestamps. */
            effective=(s.features&PG_FEATURE_THREADCTL) ?
                (s.worker_effect_ns[0]>s.worker_effect_ns[1]?s.worker_effect_ns[0]:s.worker_effect_ns[1]) :
                s.last_effective_ns;
            break;
        }
        pause_ms(1);
    }
    uint64_t last=atomic_load_explicit(&progress->progressed_ns,memory_order_acquire);
    uint64_t forced=pg_progress_forced(progress);
    int ok=observed && forced>=last+PG_PROGRESS_LIMIT && forced-last<=270*PG_MS &&
           observed-last<=270*PG_MS && effective>=last && effective<=last+270*PG_MS;
    while(now()-stopped<1000*PG_MS) pause_ms(1);
    if(kill(watched,SIGCONT)) ok=0;
    watched=0;
    for(unsigned i=0;i<20;i++) {
        pause_ms(50);
        if(!send(runtime,&rq,sizeof rq,&s,sizeof s) || !s.forced_off || s.nc1_cap_pct || s.nc2_cap_pct) ok=0;
    }
    if(!send(policy,&q,sizeof q,&p,sizeof p) || p.decision.state!=CRITICAL || !(p.decision.causes&FORCED_OFF)) ok=0;
    printf("%s supervisor_stop_ns=%" PRIu64 " last_progress_ns=%" PRIu64 " forced_ns=%" PRIu64
           " both_off_ns=%" PRIu64 " observed_ns=%" PRIu64 " observed_from_progress_ns=%" PRIu64
           " latched_after_resume=%u\n",ok?"PASS":"FAIL",stopped,last,forced,effective,observed,
           observed?observed-last:UINT64_MAX,s.forced_off);
    pg_progress_close(progress,0);name_close(runtime);name_close(policy);
    return ok?0:1;
}
