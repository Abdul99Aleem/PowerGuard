/* Host replay. All sensors, workload effects and elapsed times are simulated. */
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#e); exit(1); } } while (0)
static const char *names[] = {"Normal","Warning","Throttled","Critical","Recovery"};
typedef struct { pg_policy p; pg_sample in; uint64_t now; int verbose; } rig;

static void acknowledge(rig *r) {
    pg_status *s = &r->p.status;
    CHECK(pg_effect(&r->p, s->epoch, s->decision_id,
          s->effective_ns ? s->effective_ns : r->now,
          s->nc1_pct, s->nc2_pct, r->now));
}
static void step(rig *r, int update_thermal) {
    pg_status before = r->p.status;
    r->now += PG_TICK;
    if (update_thermal) {
        r->in.thermal_seq++;
        r->in.acquired_ns = r->now;
    }
    r->in.load_end_ns = r->now;
    pg_ingest(&r->p, &r->in, r->now);
    acknowledge(r);
    pg_status s = r->p.status;
    if (r->verbose && (s.state != before.state || s.stage != before.stage))
        printf("%6llu ms  %-10s %3u%% %3u%%  causes=0x%x\n",
               (unsigned long long)(r->now/PG_MS), names[s.state],s.nc1_pct,s.nc2_pct,s.causes);
}
static void run(rig *r, unsigned ticks) { while (ticks--) step(r, 1); }
static rig baseline(void) {
    rig r = {0};
    r.now = PG_MS;
    pg_init(&r.p,r.now);
    CHECK(r.p.status.state == CRITICAL && r.p.status.stage == 4);
    r.in.temp_deci_c = 500;
    r.in.quality = GOOD;
    r.in.source = SIMULATED;
    run(&r,320);
    CHECK(r.p.status.state == NORMAL);
    return r;
}

void metric_checks(void);
int main(void) {
    metric_checks();
    rig r = baseline();
    r.verbose = 1;
    puts("SIMULATED  gradual heat and staged recovery       NC1  NC2");
    r.in.temp_deci_c = 649; step(&r,1); CHECK(r.p.status.state == NORMAL);
    for (int t=650; t<=750; t++) {
        r.in.temp_deci_c = t; step(&r,1);
        CHECK(r.p.status.stage == (unsigned)(t>=750 ? 4 : t>=700 ? 2 : 0));
        if (t<700) CHECK(r.p.status.state == WARNING && r.p.status.nc2_pct == 100);
    }
    r.in.temp_deci_c = 620;
    run(&r,200); CHECK(r.p.status.stage == 4);
    step(&r,1); CHECK(r.p.status.state == RECOVERY && r.p.status.stage == 2);
    r.in.temp_deci_c = 630; step(&r,1);
    CHECK(r.p.status.state == CRITICAL && r.p.status.stage == 4 && !r.p.status.clear_since_ns);
    r.in.temp_deci_c = 620;
    run(&r,201); CHECK(r.p.status.stage == 2);
    run(&r,100); CHECK(r.p.status.stage == 2);
    step(&r,1); CHECK(r.p.status.stage == 0);
    step(&r,1); CHECK(r.p.status.state == NORMAL);

    puts("SIMULATED  sudden CPU spike on one core");
    r.in.core_busy_pct[2] = 86; step(&r,1);
    CHECK(r.p.status.state == THROTTLED && r.p.status.causes == CPU);
    r.in.core_busy_pct[2] = 0; run(&r,51); CHECK(r.p.status.state == RECOVERY && r.p.status.stage == 2);
    r.in.core_busy_pct[2] = 96; step(&r,1); CHECK(r.p.status.state == CRITICAL);

    r = baseline(); r.verbose = 1;
    puts("SIMULATED  same sample replayed by a live sender");
    for (int i=0; i<14; i++) step(&r,0);
    CHECK(r.p.status.state == NORMAL);
    step(&r,0); CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & BAD_TEMP));
    r.in.quality = FROZEN; step(&r,1); CHECK(r.p.status.causes & BAD_TEMP);
    r.in.quality = GOOD; run(&r,320); CHECK(r.p.status.state == NORMAL);

    r = baseline();
    for (int i=0;i<4;i++) r.in.core_busy_pct[i] = 60;
    r.in.nc_demand_pct[0] = r.in.nc_demand_pct[1] = 80;
    step(&r,1); CHECK(r.p.status.causes == POWER && r.p.status.power_index == 80);
    CHECK(!pg_effect(&r.p,r.p.status.epoch,r.p.status.decision_id-1,r.now,100,0,r.now));
    r = baseline();
    pg_tick(&r.p,r.now + PG_CPU_MAX_AGE + 1);
    CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & BAD_LOAD));
    r = baseline();
    r.in.acquired_ns += PG_TICK; /* same sequence, rewritten acquisition time */
    step(&r,0); CHECK(r.p.status.causes & BAD_TEMP);
    r = baseline();
    r.in.core_busy_pct[0]=85;
    pg_ingest(&r.p,&r.in,r.now);
    CHECK(r.p.status.stage == 2);
    pg_tick(&r.p,r.now+PG_RESPONSE_TARGET);
    CHECK(r.p.status.state == THROTTLED);
    pg_tick(&r.p,r.now+PG_RESPONSE_TARGET+1);
    CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & WAIT_EFFECT));
    r = baseline();
    pg_tick(&r.p, r.now + PG_LEASE);
    CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & WAIT_EFFECT));
    r = baseline();
    r.in.quality = FROZEN; step(&r,1);
    CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & BAD_TEMP));
    r = baseline();
    r.in.source = REAL;
    r.in.thermal_seq++; r.in.acquired_ns = r.now + 2*PG_TICK;
    step(&r,0); CHECK(r.p.status.causes & BAD_TEMP);
    r = baseline();
    r.in.core_busy_pct[0] = 101; step(&r,1);
    CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & BAD_LOAD));
    r = baseline();
    r.in.core_busy_pct[1] = 85; step(&r,1);
    CHECK(r.p.status.stage == 2);
    r.in.core_busy_pct[1] = 95; step(&r,1);
    CHECK(r.p.status.stage == 4);
    /* The fresh arrival wins dispatch, but the previous acquisition expired first. */
    r = baseline();
    for (int i=0; i<14; ++i) step(&r,0);
    step(&r,1);
    CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & BAD_TEMP));
    CHECK(r.p.status.nc1_pct == 0 && r.p.status.nc2_pct == 0);

    r = baseline(); r.in.quality = READ_ONLY;
    step(&r,1); run(&r,199); CHECK(r.p.status.state == NORMAL);
    step(&r,1); CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & BAD_TEMP));
    r.in.temp_deci_c++; run(&r,2); CHECK(!(r.p.status.causes & BAD_TEMP));
    r = baseline(); r.in.temp_deci_c = 650; step(&r,1);
    CHECK(r.p.status.state == WARNING && r.p.status.stage == 0);
    r.in.temp_deci_c = 620; run(&r,200); CHECK(r.p.status.state == WARNING);
    step(&r,1); CHECK(r.p.status.state == NORMAL);
    CHECK(pg_next_deadline(&r.p) == r.now + PG_LEASE);
    pg_tick(&r.p, r.now + PG_LEASE);
    CHECK(r.p.status.state == CRITICAL && pg_next_deadline(&r.p) == r.now + PG_LEASE + PG_RESPONSE_TARGET + 1);

    r = baseline();
    pg_metrics metrics = {0};
    pg_status before = r.p.status;
    pg_observe(&metrics, &before, &r.p);
    r.now += 18 * PG_MS;
    ++r.in.thermal_seq;
    r.in.acquired_ns = r.now;
    r.in.load_end_ns = r.now - 15 * PG_MS;
    r.in.core_busy_pct[0] = 85;
    pg_ingest(&r.p, &r.in, r.now);
    pg_observe(&metrics, &before, &r.p);
    before = r.p.status;
    r.now += 8 * PG_MS;
    acknowledge(&r);
    pg_observe(&metrics, &before, &r.p);
    CHECK(metrics.scores.late == 1 && metrics.scores.worst_response_ns == 23 * PG_MS);
    puts("PASS: host policy replay assertions");
    return 0;
}
