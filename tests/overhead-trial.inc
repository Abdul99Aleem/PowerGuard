#include "overhead-shared.h"
#include <devctl.h>
#include <sys/procfs.h>
#include <sched.h>
#include "cli-assets.h"

static char mode;
static int smoke;
static pid_t monitor_pid=-1, watchdog_pid=-1;
static overhead_shared *stats;
static const uint64_t sample_period=25*PG_MS;
static uint64_t nsclock(clockid_t c) {
    struct timespec t;
    if(clock_gettime(c,&t)) {perror("read clock");exit(2);}
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static struct timespec timespec_ns(uint64_t n) {
    return (struct timespec){(time_t)(n/1000000000),(long)(n%1000000000)};
}
static void wait_until(uint64_t when) {
    struct timespec t=timespec_ns(when);
    while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)==EINTR) {}
}
static int start_sampler_timer(uint64_t start) {
    int chid=ChannelCreate(_NTO_CHF_PRIVATE);
    int co=chid<0 ? -1 : ConnectAttach(0,0,chid,_NTO_SIDE_CHANNEL,0);
    struct sigevent event;
    SIGEV_PULSE_INIT(&event,co,10,_PULSE_CODE_MINAVAIL,0);
    timer_t timer;
    struct itimerspec period={.it_value=timespec_ns(start),.it_interval=timespec_ns(sample_period)};
    struct itimerspec tolerance={.it_value={.tv_nsec=1}};
    if(co<0 || timer_create(CLOCK_MONOTONIC,&event,&timer) ||
       timer_settime(timer,TIMER_TOLERANCE,&tolerance,NULL) ||
       timer_settime(timer,TIMER_ABSTIME,&period,NULL)) {perror("acquisition timer setup");return -1;}
    return chid;
}
static int monitor_loop(void) {
    int co=name_open(PG_NAME,0), fd=open("quiet-replay.bin",O_RDONLY);
    struct sched_param sp={.sched_priority=10};
    if(co<0 || fd<0 || sched_setscheduler(0,SCHED_RR,&sp) ||
       ThreadCtl(_NTO_TCTL_RUNMASK,(void *)(uintptr_t)2)) return 2;
    uint64_t due=nsclock(CLOCK_MONOTONIC)+sample_period;
    int chid=mode=='A' ? -1 : start_sampler_timer(due);
    if(mode!='A' && chid<0) return 2;
    unsigned sequence=0,previous_band=~0u,last_notice=0;
    for(;;due+=sample_period) {
        if(mode=='A') {while(nsclock(CLOCK_MONOTONIC)<due) {}}
        else {
            struct _pulse pulse;
            if(MsgReceivePulse(chid,&pulse,sizeof pulse,NULL)<0) return 2;
        }
        uint64_t now=nsclock(CLOCK_MONOTONIC);
        if(now>=due+sample_period) {
            unsigned lost=(unsigned)((now-due)/sample_period);
            stats->skipped+=lost; due+=lost*sample_period;
        }
        pg_sample sample;
        if(pread(fd,&sample,sizeof sample,0)!=(ssize_t)sizeof sample) return 2;
        sample.acquired_ns=sample.load_end_ns=nsclock(CLOCK_MONOTONIC);
        sample.thermal_seq=++sequence;
        int peak=0; for(int i=0;i<4;i++) if(sample.core_busy_pct[i]>peak) peak=sample.core_busy_pct[i];
        unsigned band=(unsigned)(pg_activation_limit(0,sample.temp_deci_c)+1) |
            ((unsigned)(pg_activation_limit(1,peak)+1)<<12) | ((unsigned)sample.quality<<24);
        pthread_mutex_lock(&stats->lock);
        stats->sample=sample;
        pthread_mutex_unlock(&stats->lock);
        ++stats->reads;
        if(mode!='C' || !stats->steady || band!=previous_band || sequence-last_notice>=4) {
            if(MsgSendPulse(co,10,SENSOR_PULSE,0)!=0) return 2;
            ++stats->sent; last_notice=sequence;
        }
        previous_band=band;
    }
}
/* An advancing policy timestamp proves that the policy receive loop made progress. */
static int watchdog_loop(void) {
    int co=name_open(PG_NAME,0),bc=name_open(PG_BENCH_NAME,0);
    if(co<0 || bc<0) return 2;
    pg_message_v1 m={.type=PG_READ,.version=PG_VERSION};
    pg_reply_v1 p; uint64_t last=nsclock(CLOCK_MONOTONIC), epoch=0,id=0;
    for(uint64_t due=last;;due+=50*PG_MS) {
        wait_until(due);
        uint64_t timeout=10*PG_MS;
        if(TimerTimeout(CLOCK_MONOTONIC,_NTO_TIMEOUT_SEND|_NTO_TIMEOUT_REPLY,NULL,&timeout,NULL)<0) return 2;
        if(MsgSend(co,&m,sizeof m,&p,sizeof p)>=0 && p.decision.as_of_ns>last) {
            last=p.decision.as_of_ns; epoch=p.decision.epoch; id=p.decision.decision_id;
        }
        ++stats->checks;
        uint64_t now=nsclock(CLOCK_MONOTONIC);
        if(now-last>=200*PG_MS && epoch) {
            pg_cap_request_t off={{PG_BENCH_TYPE,PG_BENCH_VERSION,PG_CMD_LEASED_CAPS,0,0},epoch,id,now+PG_LEASE};
            pg_status_t reply;
            if(TimerTimeout(CLOCK_MONOTONIC,_NTO_TIMEOUT_SEND|_NTO_TIMEOUT_REPLY,NULL,&timeout,NULL)<0 ||
               MsgSend(bc,&off,sizeof off,&reply,sizeof reply)<0 || reply.error) return 2;
            ++stats->trips;
        }
    }
}
static void stop_aux(void) {
    if(watchdog_pid>0) {kill(watchdog_pid,SIGTERM);reap(watchdog_pid);watchdog_pid=-1;}
    if(monitor_pid>0) {kill(monitor_pid,SIGTERM);reap(monitor_pid);monitor_pid=-1;}
}
static int start_aux(void) {
    monitor_pid=fork();
    if(!monitor_pid) {int rc=monitor_loop();perror("monitor exit");_exit(rc);}
    if(monitor_pid<0) return 0;
    atexit(stop_aux);
    watchdog_pid=fork();
    if(!watchdog_pid) _exit(watchdog_loop());
    return watchdog_pid>0;
}
static int prepare_overhead(void) {
    stats=open_overhead(1);
    pg_sample record={.temp_deci_c=450,.quality=GOOD,.source=SIMULATED,
        .core_busy_pct={13,7,18,7},.nc_demand_pct={20,15}};
    return stats && !stage("quiet-replay.bin",(unsigned char *)&record,sizeof record) &&
        !stage("pg-console",asset_console,sizeof asset_console);
}
struct counts {unsigned reads,sent,received,checks,skipped,trips;};
static struct counts counts_now(void) {
    return (struct counts){stats->reads,stats->sent,stats->received,stats->checks,stats->skipped,stats->trips};
}
static int idle_ids(clockid_t *ids) {
    int fd=open("/proc/1/as",O_RDONLY); if(fd<0) return 0;
    unsigned found=0;
    for(int tid=1;tid<128 && found!=15;) {
        procfs_status s={.tid=tid};
        if(devctl(fd,DCMD_PROC_TIDSTATUS,&s,sizeof s,NULL)) break;
        tid=s.tid+1;
        if(s.priority || s.last_cpu>=4) continue;
        int id=ClockId(1,s.tid); if(id==-1) break;
        ids[s.last_cpu]=id; found|=1u<<s.last_cpu;
    }
    close(fd);return found==15;
}
static int measure_overhead(const pg_reply_v1 *initial) {
    enum { COUNT=9 };
    clockid_t clk[COUNT]={ClockId(monitor_pid,1),ClockId(supervisor,1),ClockId(manager,1),
                          ClockId(watchdog_pid,1),ClockId(bench,1)};
    if(!idle_ids(clk+5)) return 2;
    for(int i=0;i<COUNT;i++) if(clk[i]==-1) return 2;
    char *args[]={"./pg-console","--live","scores",NULL};
    pid_t cli_child=launch(args);int status;
    if(cli_child<0 || waitpid(cli_child,&status,0)!=cli_child || !WIFEXITED(status) || WEXITSTATUS(status)) return 2;
    stats->steady=1;
    pause_ms(smoke ? 500 : 30000);
    pg_reply_v1 p;
    uint64_t first[COUNT],delta[COUNT],begin=nsclock(CLOCK_MONOTONIC);
    for(int i=0;i<COUNT;i++) first[i]=nsclock(clk[i]);
    struct counts a=counts_now();
    wait_until(begin+(smoke?2:60)*1000*PG_MS);
    struct counts b=counts_now();
    for(int i=0;i<COUNT;i++) delta[i]=nsclock(clk[i])-first[i];
    uint64_t end=nsclock(CLOCK_MONOTONIC), elapsed=end-begin, cpu=0;
    for(int i=0;i<5;i++) cpu+=delta[i];
    pg_status_t workers;
    int normal=read_policy(&p)>=0 && p.decision.epoch==initial->decision.epoch &&
       p.decision.decision_id==initial->decision.decision_id && p.decision.state==NORMAL &&
       p.decision.effective_ns && bench_command(PG_CMD_STATUS,0,&workers)>=0 &&
       workers.nc1_cap_pct==100 && workers.nc2_cap_pct==100 && workers.critical_fifo_ok;
    printf("MEASURE mode=%c begin_ns=%" PRIu64 " end_ns=%" PRIu64 " wall_ns=%" PRIu64
           " cpu_ns=%" PRIu64 " core_pct=%.5f reads=%u notices=%u handled=%u wd_checks=%u wd_trips=%u skipped=%u normal=%d\n",
           mode,begin,end,elapsed,cpu,100.0*cpu/elapsed,b.reads-a.reads,b.sent-a.sent,b.received-a.received,
           b.checks-a.checks,b.trips-a.trips,b.skipped-a.skipped,normal);
    printf("ROLE_CPU_NS monitor=%" PRIu64 " supervisor=%" PRIu64 " manager=%" PRIu64 " watchdog=%" PRIu64 " bench_ipc=%" PRIu64 "\n",
           delta[0],delta[1],delta[2],delta[3],delta[4]);
    printf("LIVE_CORE_BUSY_PCT %.2f %.2f %.2f %.2f\n",100.0*(1.0-(double)delta[5]/elapsed),
           100.0*(1.0-(double)delta[6]/elapsed),100.0*(1.0-(double)delta[7]/elapsed),100.0*(1.0-(double)delta[8]/elapsed));
    int count=smoke?80:2400, notices=mode=='C'?count/4:count;
    int valid=normal && !b.trips && b.skipped==a.skipped && abs((int)(b.reads-a.reads)-count)<=1 &&
         abs((int)(b.received-a.received)-notices)<=1 && abs((int)(b.sent-a.sent)-notices)<=1 &&
         abs((int)(b.checks-a.checks)-(smoke?40:1200))<=1;
    stop_aux();stop();
    int jobs=check_jobs(begin,end);
    struct stat st;
    if(stat("/proc/ham/.info",&st)==0 || errno!=ENOENT) return 2;
    printf("HAM postflight off. Window valid=%d critical_deadlines=%s budget=%s\n",
           valid,jobs?"FAIL":"PASS",100.0*cpu<=2.0*elapsed?"PASS":"FAIL");
    return valid?0:2;
}
