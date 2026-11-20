/* Independent command/effect path: never waits for telemetry manager progress. */
#include "ipc.h"
#include "pgbench_protocol.h"
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>

static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static int send_timed(int coid, const void *q, size_t size, void *r, size_t capacity) {
    uint64_t timeout = 2 * PG_MS;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                        NULL, &timeout, NULL) != -1 && MsgSend(coid,q,size,r,capacity) != -1;
}
int main(void) {
    pg_enforcer_request ping = {.request = {PG_BENCH_TYPE,PG_BENCH_VERSION,PG_CMD_ENFORCER,0,0}};
    if (pg_thread_configure(0,24,"pg-enforcer",&ping.placement)) { perror("enforcer qualification"); return 1; }
    setvbuf(stdout,NULL,_IOLBF,0);
    pg_thread_print(&ping.placement);
    int policy = name_open(PG_NAME,0), runtime = name_open(PG_BENCH_NAME,0);
    if (policy == -1 || runtime == -1) { perror("enforcer connect"); return 1; }
    signal(SIGINT,stop); signal(SIGTERM,stop);
    uint64_t logged_epoch=0, logged_id=0;
    int last_state=-1, last_recovery=-1, result=0;
    puts("CONFIRM,epoch,decision_id,restriction,detected_ns,effective_ns,received_ns,effect_latency_ns,confirmation_latency_ns,causes");
    while (running) {
        if (pg_thread_verify(&ping.placement) || ping.placement.runmask!=1 ||
            ping.placement.inherit_mask!=1 || ping.placement.cpu!=0 ||
            ping.placement.policy!=SCHED_FIFO || ping.placement.priority!=24) {
            fputs("Enforcer placement/scheduling drift\n",stderr); result=1; break;
        }
        pg_status_t bench;
        pg_message_v1 q = {.type=PG_READ,.version=PG_VERSION};
        pg_reply_v1 snapshot;
        if (!send_timed(runtime,&ping,sizeof(ping),&bench,sizeof(bench)) || bench.error ||
            !send_timed(policy,&q,sizeof(q),&snapshot,sizeof(snapshot))) {
            perror("enforcer heartbeat/read"); result=1; break;
        }
        pg_status want=snapshot.decision;
        pg_cap_request_t grant={.request={PG_BENCH_TYPE,PG_BENCH_VERSION,PG_CMD_LEASED_CAPS,
                                        (int)want.nc1_pct,(int)want.nc2_pct},
                                .epoch=want.epoch,.decision_id=want.decision_id,.until_ns=want.lease_until_ns};
        if (!send_timed(runtime,&grant,sizeof(grant),&bench,sizeof(bench)) ||
            (bench.error && bench.error!=ESTALE)) {
            perror("enforcer cap delivery"); result=1; break;
        }
        int accepted=!bench.error;
        int state=(int)want.state+PG_ST_NORMAL;
        int recovery=want.state==RECOVERY?(int)want.nc1_pct:0;
        if (state!=last_state || recovery!=last_recovery) {
            pg_request_t report={PG_BENCH_TYPE,PG_BENCH_VERSION,PG_CMD_STATE,state,recovery};
            pg_status_t ignored;
            if (!send_timed(runtime,&report,sizeof(report),&ignored,sizeof(ignored)) || ignored.error) {
                perror("enforcer state report"); result=1; break;
            }
            last_state=state; last_recovery=recovery;
        }
        if (accepted && bench.applied_epoch==want.epoch && bench.applied_decision_id==want.decision_id &&
            bench.nc1_cap_pct==(int)want.nc1_pct && bench.nc2_cap_pct==(int)want.nc2_pct &&
            bench.last_effective_ns>=want.issued_ns) {
            q=(pg_message_v1){.type=PG_EFFECT_CONFIRMED,.version=PG_VERSION};
            q.body.effect.epoch=want.epoch; q.body.effect.decision_id=want.decision_id;
            q.body.effect.effective_ns=bench.last_effective_ns;
            q.body.effect.nc1_pct=want.nc1_pct; q.body.effect.nc2_pct=want.nc2_pct;
            pg_confirmation_reply receipt;
            if (!send_timed(policy,&q,sizeof(q),&receipt,sizeof(receipt))) {
                if (errno!=EINVAL) { perror("enforcer effect receipt"); result=1; break; }
            } else if (logged_epoch!=want.epoch || logged_id!=want.decision_id) {
                if (receipt.received_ns<receipt.detected_ns || want.epoch!=receipt.snapshot.decision.epoch ||
                    want.decision_id!=receipt.snapshot.decision.decision_id) {
                    fputs("Invalid supervisor effect receipt\n",stderr); result=1; break;
                }
                printf("CONFIRM,%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 ",%" PRIu64 ",%u\n",want.epoch,want.decision_id,receipt.restriction,
                       receipt.detected_ns,receipt.snapshot.decision.effective_ns,receipt.received_ns,
                       receipt.snapshot.decision.effective_ns-receipt.detected_ns,
                       receipt.received_ns-receipt.detected_ns,receipt.snapshot.decision.causes);
                logged_epoch=want.epoch; logged_id=want.decision_id;
            }
        }
        uint64_t tolerance=1;
        struct timespec pause={.tv_nsec=2*PG_MS};
        if (TimerTimeout(CLOCK_MONOTONIC,TIMER_TOLERANCE,NULL,&tolerance,NULL)==-1) {
            perror("enforcer timer tolerance"); result=1; break;
        }
        while (running && nanosleep(&pause,&pause)==-1 && errno==EINTR) {}
    }
    name_close(runtime); name_close(policy);
    return result;
}
