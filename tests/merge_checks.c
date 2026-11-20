#include "pg_progress.h"
#include "pg_qualification.h"
#include "policy.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
int main(void) {
    pg_progress p;
    atomic_init(&p.version,PG_PROGRESS_VERSION); atomic_init(&p.epoch,0);
    atomic_init(&p.progressed_ns,0); atomic_init(&p.forced_ns,0);
    uint64_t start=1000*PG_MS;
    pg_progress_publish(&p,1,start);
    CHECK(!pg_progress_stale(&p,start+PG_PROGRESS_LIMIT-1));
    CHECK(pg_progress_stale(&p,start+PG_PROGRESS_LIMIT));
    pg_progress_trip(&p,start+PG_PROGRESS_LIMIT);
    uint64_t trip=pg_progress_forced(&p);
    pg_progress_publish(&p,2,start+PG_PROGRESS_LIMIT+PG_MS);
    pg_progress_trip(&p,start+PG_PROGRESS_LIMIT+2*PG_MS);
    CHECK(pg_progress_forced(&p)==trip);
    CHECK(!pg_progress_allows(&p,100,100) && !pg_progress_allows(&p,100,0));
    CHECK(pg_progress_allows(&p,0,0));
    CHECK(pg_overhead_result('C',1,1,1,100)==1); /* one missed release fails */
    CHECK(pg_overhead_result('B',1,0,3,100)==1);
    CHECK(pg_overhead_result('C',1,0,3,100)==1);
    CHECK(pg_overhead_result('C',1,0,2,100)==0);
    CHECK(pg_overhead_result('A',1,0,99,100)==0); /* intentional busy reference */
    CHECK(pg_overhead_result('A',1,1,99,100)==1);
    CHECK(pg_overhead_result('C',0,0,1,100)==2);

    pg_policy policy; pg_init(&policy,start);
    policy.forced_off_ns=trip;
    for (unsigned i=1;i<401;++i) {
        uint64_t n=start+PG_PROGRESS_LIMIT+i*PG_TICK;
        pg_sample s={.thermal_seq=i,.acquired_ns=n,.load_end_ns=n,
                     .temp_deci_c=450,.quality=GOOD,.source=SIMULATED};
        pg_ingest(&policy,&s,n);
        CHECK(pg_effect(&policy,policy.status.epoch,policy.status.decision_id,
            policy.status.effective_ns?policy.status.effective_ns:n,0,0,n));
        CHECK(policy.status.state==CRITICAL && (policy.status.causes&FORCED_OFF));
        CHECK(!policy.status.nc1_pct && !policy.status.nc2_pct && !policy.status.clear_since_ns);
    }
    puts("PASS watchdog deadline, immutable latch, blocked restoration, and benchmark failure gates");
    return 0;
}
