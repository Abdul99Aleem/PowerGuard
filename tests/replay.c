/* Host replay. All sensors, workload effects and elapsed times are simulated. */
#include "policy.h"
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
/* Enough ticks for a full recovery from Critical, with a little slack. */
#define RECOVER_TICKS ((unsigned)((PG_RESTORE_NC1 + PG_RESTORE_NC2) / PG_TICK) + 10)
static rig baseline(void) {
    rig r = {0};
    r.now = PG_MS;
    pg_init(&r.p,r.now);
    CHECK(r.p.status.state == CRITICAL && r.p.status.stage == CRITICAL);
    r.in.temp_deci_c = 500;
    r.in.quality = GOOD;
    r.in.source = SIMULATED;
    r.in.verified = 1;
    run(&r,RECOVER_TICKS);
    CHECK(r.p.status.state == NORMAL);
    return r;
}

enum { SIGNAL_TEMP, SIGNAL_CPU, SIGNAL_POWER };

static void set_signal(rig *r, int signal, int value) {
    switch (signal) {
    case SIGNAL_TEMP: r->in.temp_deci_c = value; break;
    case SIGNAL_CPU: r->in.core_busy_pct[0] = value; break;
    default: r->in.nc_demand_pct[0] = r->in.nc_demand_pct[1] = value; break;
    }
}

static unsigned stage_for(int signal, int value) {
    rig r = baseline();
    set_signal(&r, signal, value);
    step(&r, 1);
    return r.p.status.stage;
}

/* Every limit is inclusive: one below it changes nothing, the limit itself acts. */
static void limit_boundaries(void) {
    static const int limit[3][3] = {
        {PG_TEMP_WARN, PG_TEMP_THROTTLE, PG_TEMP_CRITICAL},
        {PG_CPU_WARN, PG_CPU_THROTTLE, PG_CPU_CRITICAL},
        {PG_POWER_WARN, PG_POWER_THROTTLE, PG_POWER_CRITICAL},
    };
    static const unsigned stage[3] = {WARNING, THROTTLED, CRITICAL};
    for (int signal = 0; signal < 3; ++signal)
        for (int level = 0; level < 3; ++level) {
            unsigned below = level ? stage[level - 1] : NORMAL;
            CHECK(stage_for(signal, limit[signal][level] - 1) == below);
            CHECK(stage_for(signal, limit[signal][level]) == stage[level]);
        }
}

/* Three severities at once: the strictest cap wins and every cause is kept. */
static void simultaneous_faults(void) {
    rig r = baseline();
    set_signal(&r, SIGNAL_TEMP, PG_TEMP_THROTTLE);
    set_signal(&r, SIGNAL_CPU, PG_CPU_CRITICAL);
    set_signal(&r, SIGNAL_POWER, PG_POWER_WARN);
    step(&r, 1);
    CHECK(r.p.status.stage == CRITICAL && r.p.status.state == CRITICAL);
    CHECK(r.p.status.causes == (TEMP | CPU | POWER));
}

/* P = 0.4 NC1 + 0.6 NC2, rounded down (PRD page 3). */
static void power_index(void) {
    rig r = baseline();
    r.in.nc_demand_pct[0] = 20; r.in.nc_demand_pct[1] = 90;
    /* J2: 62 is a Warning pre-alarm, below the actionable Throttled limit. */
    step(&r, 1);
    CHECK(r.p.status.power_index == 62 && r.p.status.causes == POWER && r.p.status.stage == WARNING);
    r.in.nc_demand_pct[0] = 25; r.in.nc_demand_pct[1] = 25;
    step(&r, 1); CHECK(r.p.status.power_index == 25);
    r.in.nc_demand_pct[0] = 1; r.in.nc_demand_pct[1] = 0;
    step(&r, 1); CHECK(r.p.status.power_index == 0);
    /* J5: both requests at 80 with U = 40 is Throttled on power alone. */
    r.in.core_busy_pct[0] = 40;
    r.in.nc_demand_pct[0] = r.in.nc_demand_pct[1] = 80;
    step(&r, 1);
    CHECK(r.p.status.power_index == 80 && r.p.status.causes == POWER && r.p.status.stage == THROTTLED);
}

/* Restoration timeline, in ms from the first all-clear sample. */
typedef struct { uint64_t nc1_ms, full_ms, normal_ms; } timeline;

static timeline clear_until_normal(rig *r) {
    timeline t = {0, 0, 0};
    r->in.temp_deci_c = PG_TEMP_CLEAR;
    step(r, 1);
    uint64_t start = r->now;
    CHECK(r->p.status.clear_since_ns == start);
    while (r->p.status.state != NORMAL) {
        CHECK(r->now - start < 5000 * PG_MS);
        step(r, 1);
        uint64_t at = (r->now - start) / PG_MS;
        if (!t.nc1_ms && r->p.status.nc1_pct == 100) t.nc1_ms = at;
        if (!t.full_ms && r->p.status.nc2_pct == 100) t.full_ms = at;
    }
    t.normal_ms = (r->now - start) / PG_MS;
    return t;
}

static rig after(int temp) {
    rig r = baseline();
    r.in.temp_deci_c = temp;
    step(&r, 1);
    return r;
}

/* J4: from Critical, NC1 returns at 2.0 s and NC2 a further 1.0 s later. */
static void controlled_restoration(void) {
    rig r = after(PG_TEMP_CRITICAL);
    timeline t = clear_until_normal(&r);
    CHECK(t.nc1_ms == 2000 && t.full_ms == 3000 && t.normal_ms <= 3100);

    /* A one-sample excursion at 1.99 s restarts the whole wait. */
    r = after(PG_TEMP_CRITICAL);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    step(&r, 1);
    while (r.now - r.p.status.clear_since_ns < 1980 * PG_MS) step(&r, 1);
    CHECK(r.p.status.nc1_pct == 0 && r.p.status.nc2_pct == 0);
    r.in.temp_deci_c = PG_TEMP_CLEAR + 1;
    step(&r, 1);
    CHECK(r.p.status.state == CRITICAL && !r.p.status.clear_since_ns);
    t = clear_until_normal(&r);
    CHECK(t.nc1_ms == 2000 && t.full_ms == 3000);

    /* An excursion just after NC1 reopens closes it again. */
    r = after(PG_TEMP_CRITICAL);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    while (r.p.status.nc1_pct == 0) step(&r, 1);
    CHECK(r.p.status.state == RECOVERY && r.p.status.nc2_pct == 0);
    r.in.temp_deci_c = PG_TEMP_CLEAR + 1;
    step(&r, 1);
    CHECK(r.p.status.state == CRITICAL && r.p.status.nc1_pct == 0);
}

/* From Throttled NC1 is already open, but NC2 still waits the full 3.0 s. */
static void throttled_restoration(void) {
    rig r = after(PG_TEMP_THROTTLE);
    CHECK(r.p.status.state == THROTTLED && r.p.status.nc1_pct == 100 && r.p.status.nc2_pct == 0);
    timeline t = clear_until_normal(&r);
    CHECK(t.full_ms == 3000 && t.normal_ms <= 3100);

    /* A cancel returns to Throttled without reissuing the same caps. */
    r = after(PG_TEMP_THROTTLE);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    run(&r, 100);
    uint64_t id = r.p.status.decision_id;
    CHECK(r.p.status.state == RECOVERY);
    r.in.temp_deci_c = PG_TEMP_CLEAR + 1;
    step(&r, 1);
    CHECK(r.p.status.state == THROTTLED && r.p.status.decision_id == id);
}

/* Warning sheds nothing, so it returns straight to Normal after 2.0 s. */
static void warning_return(void) {
    rig r = after(PG_TEMP_WARN);
    CHECK(r.p.status.state == WARNING && r.p.status.nc1_pct == 100 && r.p.status.nc2_pct == 100);
    r.in.temp_deci_c = PG_TEMP_CLEAR + 1; /* in the gap: Warning holds */
    run(&r, 300);
    CHECK(r.p.status.state == WARNING && !r.p.status.clear_since_ns);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    step(&r, 1);
    uint64_t start = r.now;
    while (r.p.status.state == WARNING) step(&r, 1);
    CHECK(r.p.status.state == NORMAL && r.now - start == PG_WARNING_CLEAR);
}

/* Resend the same reading at a chosen time, as a quiet heartbeat would. */
static void resend_at(rig *r, uint64_t at) {
    r->now = at;
    r->in.load_end_ns = at;
    pg_ingest(&r->p, &r->in, at);
    acknowledge(r);
}

/* J3a: 150 ms without a new valid acquisition is Critical, whatever arrives. */
static void age_expiry(void) {
    rig r = baseline();
    uint64_t last = r.in.acquired_ns, deadline = last + PG_TEMP_MAX_AGE;
    CHECK(pg_age_deadline(&r.p) == deadline);
    for (uint64_t t = last + PG_TICK; t < deadline; t += PG_TICK) resend_at(&r, t);
    resend_at(&r, deadline - 1);
    CHECK(r.p.status.state == NORMAL);
    resend_at(&r, deadline);
    CHECK(r.p.status.state == CRITICAL && r.p.status.causes == TEMP_EXPIRED);

    /* A record taken before the deadline but delivered after it cannot
       undo the expiry. */
    r.in.thermal_seq++;
    r.in.acquired_ns = deadline - PG_MS;
    resend_at(&r, deadline + 5 * PG_MS);
    CHECK((r.p.status.causes & TEMP_EXPIRED) && pg_age_deadline(&r.p) == deadline);

    /* A fresh reading after that starts an ordinary recovery. */
    timeline t = clear_until_normal(&r);
    CHECK(t.nc1_ms == 2000 && t.full_ms == 3000);
}

/* B1: when an expiry and a restore fall on the same instant, the expiry wins. */
static void expiry_beats_restore(void) {
    rig r = after(PG_TEMP_WARN);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    step(&r, 1);
    uint64_t back_to_normal = r.now + PG_WARNING_CLEAR;
    while (r.now < back_to_normal - PG_TEMP_MAX_AGE) step(&r, 1);
    while (r.now < back_to_normal) {
        CHECK(r.p.status.state == WARNING);
        step(&r, 0);
    }
    CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & TEMP_EXPIRED));
}

/* A sampling gap over 35 ms breaks a recovery dwell; 30 ms does not. */
static void recovery_gap(void) {
    rig r = after(PG_TEMP_CRITICAL);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    step(&r, 1);
    uint64_t since = r.p.status.clear_since_ns;
    step(&r, 0); step(&r, 0); step(&r, 1);
    step(&r, 0); step(&r, 0); step(&r, 0);
    CHECK(r.p.status.state == RECOVERY && r.p.status.clear_since_ns == since);
    step(&r, 0);
    CHECK(r.p.status.state == CRITICAL && !r.p.status.clear_since_ns);

    /* The same gap seen only by the next sample, with no tick in between. */
    r = after(PG_TEMP_CRITICAL);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    step(&r, 1);
    CHECK(r.p.status.state == RECOVERY);
    r.now += 40 * PG_MS;
    r.in.thermal_seq++;
    r.in.acquired_ns = r.in.load_end_ns = r.now;
    pg_ingest(&r.p, &r.in, r.now);
    CHECK(r.p.status.state == CRITICAL);
}

/* J3b: an unverified value that never changes is Critical after 2.0 s. */
static void unverified_flat(void) {
    rig r = baseline();
    r.in.verified = 0;
    r.in.temp_deci_c = 600;
    step(&r, 1);
    uint64_t start = r.now;
    while (r.now < start + PG_FLAT_LIMIT - PG_TICK) {
        step(&r, 1);
        CHECK(r.p.status.state == NORMAL);
    }
    step(&r, 1);
    CHECK(r.p.status.state == CRITICAL && r.p.status.causes == UNVERIFIED_FLAT);
    run(&r, RECOVER_TICKS);
    CHECK(r.p.status.state == CRITICAL);

    /* A changing value clears it, then the ordinary recovery runs. */
    for (unsigned i = 0; r.p.status.state != NORMAL; ++i) {
        CHECK(i < RECOVER_TICKS);
        r.in.temp_deci_c = 600 + (int)(i & 1);
        step(&r, 1);
    }

    /* The same steady 60 C is fine when every conversion is verified. */
    r = baseline();
    r.in.temp_deci_c = 600;
    run(&r, 2 * RECOVER_TICKS);
    CHECK(r.p.status.state == NORMAL && !r.p.status.causes);
}

void metric_checks(void);
int main(void) {
    metric_checks();
    rig r = baseline();
    r.verbose = 1;
    puts("SIMULATED  gradual heat and staged recovery       NC1  NC2");
    r.in.temp_deci_c = PG_TEMP_WARN - 1; step(&r,1); CHECK(r.p.status.state == NORMAL);
    for (int t=PG_TEMP_WARN; t<=PG_TEMP_CRITICAL; t++) {
        r.in.temp_deci_c = t; step(&r,1);
        CHECK(r.p.status.stage == (unsigned)(t>=PG_TEMP_CRITICAL ? CRITICAL :
                                             t>=PG_TEMP_THROTTLE ? THROTTLED : WARNING));
    }
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    run(&r,50); CHECK(r.p.status.state == RECOVERY && r.p.status.stage == CRITICAL);
    r.in.temp_deci_c = PG_TEMP_CLEAR + 1; step(&r,1);
    CHECK(r.p.status.state == CRITICAL && !r.p.status.clear_since_ns);
    r.in.temp_deci_c = PG_TEMP_CLEAR;
    run(&r,RECOVER_TICKS); CHECK(r.p.status.state == NORMAL);

    puts("SIMULATED  sudden CPU spike on one core");
    r.in.core_busy_pct[2] = 86; step(&r,1);
    CHECK(r.p.status.state == THROTTLED && r.p.status.causes == CPU);
    r.in.core_busy_pct[2] = 0; run(&r,51);
    CHECK(r.p.status.state == RECOVERY && r.p.status.stage == THROTTLED);
    r.in.core_busy_pct[2] = 96; step(&r,1); CHECK(r.p.status.state == CRITICAL);

    r = baseline(); r.verbose = 1;
    puts("SIMULATED  same sample replayed by a live sender");
    for (int i=0; i<14; i++) step(&r,0);
    CHECK(r.p.status.state == NORMAL);
    step(&r,0); CHECK(r.p.status.state == CRITICAL && (r.p.status.causes & TEMP_EXPIRED));
    r.in.quality = FROZEN; step(&r,1); CHECK(r.p.status.causes & BAD_TEMP);
    r.in.quality = GOOD; run(&r,RECOVER_TICKS); CHECK(r.p.status.state == NORMAL);

    r = baseline();
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
    r.in.core_busy_pct[0]=75;
    pg_ingest(&r.p,&r.in,r.now);
    CHECK(r.p.status.stage == WARNING);
    pg_tick(&r.p,r.now+PG_RESPONSE_TARGET);
    CHECK(r.p.status.state == WARNING);
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
    CHECK(r.p.status.stage == THROTTLED);
    r.in.core_busy_pct[1] = 95; step(&r,1);
    CHECK(r.p.status.stage == CRITICAL);
    limit_boundaries();
    simultaneous_faults();
    power_index();
    controlled_restoration();
    throttled_restoration();
    warning_return();
    age_expiry();
    expiry_beats_restore();
    recovery_gap();
    unverified_flat();
    puts("PASS: host policy replay assertions");
    return 0;
}
