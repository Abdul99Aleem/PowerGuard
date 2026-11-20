#include "policy.h"
#include <string.h>

/* Background CPU caps, ordered from full service to maximum restriction. */
static const struct {
    enum pg_state state;
    unsigned nc1_percent, nc2_percent;
} stages[] = {
    { NORMAL,    100, 100 },
    { WARNING,   100,  50 },
    { THROTTLED, 100,   0 },
    { THROTTLED,  50,   0 },  /* halfway through restoring NC1 */
    { CRITICAL,    0,   0 }
};

/* All three comparators include equality. Values in the gap hold the caps. */
typedef struct { int clear, warn, throttle, critical; } limits;
static const limits thermal = {600,650,700,780}; /* tenths of a degree C */
static const limits cpu = {60,75,85,95};
static const limits power = {55,70,80,90};

static unsigned severity(int value, limits l) {
    if (value >= l.critical) return 4;
    if (value >= l.throttle) return 2;
    return value >= l.warn ? 1 : 0;
}

int pg_activation_limit(unsigned signal, int value) {
    const limits bands[] = {thermal, cpu, power};
    if (signal >= 3) return -1;
    limits l = bands[signal];
    if (value >= l.critical) return l.critical;
    if (value >= l.throttle) return l.throttle;
    return value >= l.warn ? l.warn : -1;
}

int pg_recovery_ready(const pg_policy *p) {
    return p->status.causes == 0 && p->sample.temp_deci_c <= thermal.clear &&
           p->status.cpu_peak_pct <= (unsigned)cpu.clear &&
           p->status.power_index <= (unsigned)power.clear;
}

static void set_stage(pg_status *s, unsigned stage, uint64_t now) {
    s->stage = stage;
    s->nc1_pct = stages[stage].nc1_percent;
    s->nc2_pct = stages[stage].nc2_percent;
    ++s->decision_id;
    s->issued_ns = now;
    s->effective_ns = 0;
    s->clear_since_ns = 0;
}

/* The supervisor FSM. A larger stage gives the background workers less CPU. */
static void advance_fsm(pg_status *s, unsigned required_stage,
                        int below_recovery_limits, uint64_t now) {
    /* From any state, a fault or overload immediately enforces the tighter cap. */
    if (required_stage != 0) {
        if (required_stage > s->stage) set_stage(s, required_stage, now);
        s->state = stages[s->stage].state;
        s->clear_since_ns = 0;
        return;
    }

    /* A release requires continuous headroom and confirmation of the last cap. */
    if (!below_recovery_limits || !s->effective_ns) {
        s->clear_since_ns = 0;
        return;
    }
    if (s->state == NORMAL) return;
    if (!s->clear_since_ns) s->clear_since_ns = now;
    if (now - s->clear_since_ns < PG_DWELL) return;

    switch (s->state) {
    case CRITICAL:
    case THROTTLED:
    case WARNING:
        s->state = RECOVERY;
        set_stage(s, s->stage - 1, now);
        break;
    case RECOVERY:
        if (s->stage > 0) {
            set_stage(s, s->stage - 1, now);
        } else {
            s->state = NORMAL;
            s->clear_since_ns = 0;
        }
        break;
    case NORMAL:
        break;
    }
}

void pg_init(pg_policy *p, uint64_t now) {
    memset(p, 0, sizeof(*p));
    p->status.epoch = now;
    p->status.state = CRITICAL;
    set_stage(&p->status, 4, now);
    pg_tick(p, now);
}

static int fresh(uint64_t stamp, uint64_t now, uint64_t max_age) {
    return stamp != 0 && stamp <= now && now - stamp <= max_age;
}

static void ingest(pg_policy *p, const pg_sample *s, uint64_t now) {
    if (s->acquired_ns > now) {
        p->bad_sequence = 1;
        pg_tick(p, now);
        return;
    }
    /* A sequence identifies one acquisition. Republishing it cannot freshen it.
       A producer restart requires a new run/epoch on this draft interface. */
    if (p->have_sample && (s->thermal_seq < p->sample.thermal_seq ||
        (s->thermal_seq == p->sample.thermal_seq &&
        (s->acquired_ns != p->sample.acquired_ns ||
         s->temp_deci_c != p->sample.temp_deci_c || s->source != p->sample.source)) ||
        (s->thermal_seq > p->sample.thermal_seq &&
         s->acquired_ns <= p->sample.acquired_ns))) {
        p->bad_sequence = 1;
        p->sample.quality = SENSOR_ERROR;
        pg_tick(p, now);
        return;
    }
    if (!p->have_sample || s->thermal_seq > p->sample.thermal_seq)
        p->bad_sequence = 0;
    if (s->quality != GOOD || s->acquired_ns > now) p->bad_sequence = 1;
    if (p->have_sample && (!fresh(p->sample.acquired_ns, now, PG_TEMP_MAX_AGE) ||
        !fresh(p->sample.load_end_ns, now, PG_CPU_MAX_AGE))) {
        p->status.clear_since_ns = 0;
        p->healthy_since_ns = 0;
    }
    int bad_load_stamp = s->load_end_ns > now ||
        (p->have_sample && s->load_end_ns < p->sample.load_end_ns);
    p->sample = *s;
    if (bad_load_stamp) p->sample.load_end_ns = 0;
    p->have_sample = 1;
    pg_tick(p, now);
}

void pg_ingest(pg_policy *p, const pg_sample *s, uint64_t now) {
    p->explicit_power = 0;
    ingest(p,s,now);
}

void pg_ingest_power(pg_policy *p, const pg_sample *s, int index, uint64_t now) {
    p->explicit_power = 1;
    p->power_value = index;
    ingest(p,s,now);
}

int pg_effect(pg_policy *p, uint64_t epoch, uint64_t id, uint64_t effective,
              unsigned nc1, unsigned nc2, uint64_t now) {
    pg_status *s = &p->status;
    if (epoch != s->epoch || id != s->decision_id || !effective ||
        effective < s->issued_ns || effective > now ||
        nc1 != s->nc1_pct || nc2 != s->nc2_pct) return 0;
    if (s->effective_ns && s->effective_ns != effective) return 0;
    p->manager_seen_ns = now;
    s->effective_ns = effective;
    return 1;
}

void pg_tick(pg_policy *p, uint64_t now) {
    pg_status *s = &p->status;
    const pg_sample *in = &p->sample;
    int peak = 0, sum = 0;
    unsigned required = 0, causes = 0;
    if (now - p->last_eval_ns > PG_CPU_MAX_AGE) {
        s->clear_since_ns = 0;
        p->healthy_since_ns = 0;
    }
    p->last_eval_ns = now;
    s->as_of_ns = now;
    s->lease_until_ns = now + PG_LEASE;
    if (!p->have_sample || p->bad_sequence || in->quality != GOOD ||
        (in->source != REAL && in->source != SIMULATED) ||
        !in->thermal_seq || !fresh(in->acquired_ns, now, PG_TEMP_MAX_AGE) ||
        in->temp_deci_c < -400 || in->temp_deci_c > 1250) causes |= BAD_TEMP;
    if (!fresh(in->load_end_ns, now, PG_CPU_MAX_AGE)) causes |= BAD_LOAD;
    for (unsigned i = 0; i < 4; ++i) {
        int busy = in->core_busy_pct[i];
        if (busy < 0 || busy > 100) { causes |= BAD_LOAD; continue; }
        sum += busy;
        if (busy > peak) peak = busy;
    }
    int demand = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int requested = in->nc_demand_pct[i];
        if (requested < 0 || requested > 100) causes |= BAD_LOAD;
        else demand += requested;
    }
    s->cpu_peak_pct = (unsigned)peak;
    /* Equal weights on mean occupancy and mean pre-cap demand, rounded down. */
    int index = (sum + 2*demand)/8;
    if (p->explicit_power) {
        if (p->power_value < 0 || p->power_value > 100) causes |= BAD_LOAD;
        else index = p->power_value;
    }
    s->power_index = (unsigned)index;
    unsigned bands[] = {severity(in->temp_deci_c, thermal), severity(peak,cpu),
                        severity(index,power)};
    const unsigned flags[] = {TEMP,CPU,POWER};
    for (unsigned i = 0; i < 3; ++i) {
        if (bands[i]) causes |= flags[i];
        if (bands[i] > required) required = bands[i];
    }
    if (causes & (BAD_TEMP | BAD_LOAD)) required = 4;
    if (s->effective_ns && now - p->manager_seen_ns >= PG_LEASE) {
        set_stage(s, 4, now);
        causes |= WAIT_EFFECT;
        required = 4;
    }
    if (!s->effective_ns && now - s->issued_ns > PG_RESPONSE_TARGET) {
        causes |= WAIT_EFFECT;
        required = 4;
    }
    s->causes = causes;
    if (!pg_recovery_ready(p)) p->healthy_since_ns = 0;
    else if (!p->healthy_since_ns) p->healthy_since_ns = now;
    advance_fsm(s, required, pg_recovery_ready(p), now);
}
