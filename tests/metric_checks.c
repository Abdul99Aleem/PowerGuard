#include "metrics.h"
#include "bridge.h"
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"metric check failed at line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
void metric_checks(void) {
    REQUIRE(pg_bench_state_from_policy(NORMAL) == PG_ST_NORMAL);
    REQUIRE(pg_bench_state_from_policy(WARNING) == PG_ST_WARNING);
    REQUIRE(pg_bench_state_from_policy(THROTTLED) == PG_ST_THROTTLED);
    REQUIRE(pg_bench_state_from_policy(CRITICAL) == PG_ST_CRITICAL);
    REQUIRE(pg_bench_state_from_policy(RECOVERY) == PG_ST_RECOVERY);
    pg_metrics m={0};
    pg_policy p={0};
    pg_status *s=&p.status;
    s->state=NORMAL; s->as_of_ns=PG_MS;
    pg_status old=*s;
    pg_observe(&m,&old,&p);
    for (unsigned run=0;run<2;++run) {
        uint64_t trigger=(100+run*500)*PG_MS;
        REQUIRE(pg_trial(&m,1,NORMAL,THROTTLED,trigger,trigger));
        old=*s;
        s->state=THROTTLED; s->stage=2; ++s->decision_id;
        s->issued_ns=s->as_of_ns=trigger+(2+run)*PG_MS;
        s->effective_ns=0; s->causes=CPU; s->cpu_peak_pct=88;
        pg_observe(&m,&old,&p);
        old=*s;
        s->as_of_ns=s->effective_ns=s->issued_ns+(5+20*run)*PG_MS;
        pg_observe(&m,&old,&p);
        old=*s;
        s->as_of_ns+=PG_MS;
        s->cpu_peak_pct=92; pg_observe(&m,&old,&p);
        old=*s;
        p.healthy_since_ns=s->as_of_ns+PG_MS;
        ++s->decision_id; s->stage=0; s->state=RECOVERY; s->cpu_peak_pct=0;
        s->effective_ns=s->as_of_ns=p.healthy_since_ns+50*PG_MS;
        pg_observe(&m,&old,&p);
        old=*s; s->state=NORMAL; pg_observe(&m,&old,&p);
    }
    REQUIRE(m.scores.issued==2 && m.scores.completed==2 && m.scores.late==1);
    REQUIRE(m.scores.worst_response_ns==25*PG_MS && m.scores.overshoot[1]==7);
    pg_spread j=m.trials[0][NORMAL][THROTTLED];
    REQUIRE(j.count==2 && j.max_ns-j.min_ns==PG_MS);
    REQUIRE(m.scores.restores==2 && m.scores.last_restore_ns==50*PG_MS);
    REQUIRE(pg_job(&m,1,10,10,20,30) && pg_job(&m,2,20,10,31,40));
    REQUIRE(!pg_job(&m,2,20,10,31,40) && !pg_job(&m,3,40,10,0,50));
    REQUIRE(pg_job(&m,4,40,10,0,51));
    REQUIRE(m.scores.misses==2 && m.scores.jobs==3 && m.scores.job_gaps==1);
    pg_sample in={.quality=GOOD,.source=REAL,.thermal_seq=1,.acquired_ns=100,.load_end_ns=100};
    pg_init(&p,99);
    pg_ingest_power(&p,&in,80,100); REQUIRE(p.status.causes & POWER);
    pg_ingest_power(&p,&in,101,100); REQUIRE(p.status.causes & BAD_LOAD);
    pg_ingest(&p,&in,100); REQUIRE(!(p.status.causes & (POWER|BAD_LOAD)));
    puts("PASS: all five score calculations and independent power input");
}
