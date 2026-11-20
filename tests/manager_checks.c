#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void) {
    pg_policy p;
    uint64_t start=1000*PG_MS;
    pg_init(&p,start);
    /* Set up an already qualified Normal state so expiry is a restriction. */
    p.status.state=NORMAL; p.status.stage=0; p.status.nc1_pct=p.status.nc2_pct=100;
    p.status.issued_ns=start;
    pg_sample sample={.thermal_seq=1,.acquired_ns=start,.load_end_ns=start,
                      .temp_deci_c=450,.quality=GOOD,.source=SIMULATED};
    pg_ingest(&p,&sample,start);
    CHECK(pg_effect(&p,p.status.epoch,p.status.decision_id,start,100,100,start));
    CHECK(pg_manager_progress(&p,p.status.epoch,1,start,start));
    CHECK(!pg_manager_progress(&p,p.status.epoch,1,start+PG_MS,start+PG_MS));
    CHECK(!pg_manager_progress(&p,p.status.epoch+1,2,start+PG_MS,start+PG_MS));
    CHECK(!pg_manager_progress(&p,p.status.epoch,2,start+2*PG_MS,start+PG_MS));
    CHECK(!pg_manager_progress(&p,p.status.epoch,2,start,start+PG_MS));
    CHECK(p.manager_progress_ns==start && p.manager_progress_seq==1);
    CHECK(pg_next_deadline(&p)==start+PG_MANAGER_MAX_AGE);
    pg_metrics metrics={0}; pg_status before=p.status;
    pg_observe(&metrics,&before,&p);
    /* Keep effect reporting alive: it must NOT keep manager progress alive. */
    pg_tick(&p,start+49*PG_MS);
    CHECK(pg_effect(&p,p.status.epoch,p.status.decision_id,start,100,100,start+49*PG_MS));
    before=p.status;
    pg_tick(&p,start+PG_MANAGER_MAX_AGE);
    pg_observe(&metrics,&before,&p);
    CHECK(p.status.state==CRITICAL && (p.status.causes&BAD_MANAGER));
    CHECK(!p.status.nc1_pct && !p.status.nc2_pct && !p.status.effective_ns);
    CHECK(p.restriction_from_ns==start+PG_MANAGER_MAX_AGE);
    uint64_t id=p.status.decision_id, detected=p.restriction_from_ns;
    CHECK(!pg_effect(&p,p.status.epoch,id-1,detected,0,0,detected+PG_MS));
    before=p.status;
    CHECK(pg_effect(&p,p.status.epoch,id,detected+5*PG_MS,0,0,detected+8*PG_MS));
    pg_observe(&metrics,&before,&p);
    CHECK(p.effect_received_id==id && p.effect_received_ns==detected+8*PG_MS);
    CHECK(metrics.scores.completed==1 && metrics.scores.late==0 && metrics.scores.worst_response_ns==5*PG_MS);
    CHECK(pg_effect(&p,p.status.epoch,id,detected+5*PG_MS,0,0,detected+9*PG_MS));
    CHECK(p.effect_received_ns==detected+8*PG_MS); /* heartbeat cannot rewrite first receipt */
    pg_tick(&p,detected+9*PG_MS);
    CHECK(p.status.causes&BAD_MANAGER); /* effect reporter cannot clear fault */
    CHECK(pg_manager_progress(&p,p.status.epoch,2,detected+10*PG_MS,detected+10*PG_MS));
    CHECK(!(p.status.causes&BAD_MANAGER) && p.status.state==RECOVERY);
    puts("PASS manager progress expiry, replay rejection, independent ACKs and immutable receipt timing");
    return 0;
}
