#include "policy.h"
#include <string.h>

/* Worker caps for each restriction level. The index is the state that applies
   the level, and a higher level always leaves the workers less CPU. */
static const struct { unsigned nc1, nc2; } caps[] = {
    [NORMAL]    = {100, 100},
    [WARNING]   = {100, 100}, /* pre-alarm: nothing is shed yet */
    [THROTTLED] = {100,   0},
    [CRITICAL]  = {  0,   0},
};

/* All three comparators include equality. Values in the gap hold the caps. */
typedef struct { int clear, warn, throttle, critical; } limits;
static const limits thermal = {PG_TEMP_CLEAR, PG_TEMP_WARN, PG_TEMP_THROTTLE, PG_TEMP_CRITICAL};
static const limits cpu = {PG_CPU_CLEAR, PG_CPU_WARN, PG_CPU_THROTTLE, PG_CPU_CRITICAL};
static const limits power = {PG_POWER_CLEAR, PG_POWER_WARN, PG_POWER_THROTTLE, PG_POWER_CRITICAL};

static unsigned severity(int value, limits l) {
    if (value >= l.critical) return CRITICAL;
    if (value >= l.throttle) return THROTTLED;
    return value >= l.warn ? WARNING : NORMAL;
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

static unsigned target_of(const pg_status *s, unsigned i) {
    return i == 0 ? s->nc1_pct : s->nc2_pct;
}

/* Publish a new cap target. It counts only once each worker confirms it. A
   worker already confirmed at that cap in this epoch is carried over, which
   is how an already confirmed stop is skipped. */
static void issue(pg_status *s, unsigned level, uint64_t now) {
    s->stage = level;
    s->nc1_pct = caps[level].nc1;
    s->nc2_pct = caps[level].nc2;
    ++s->decision_id;
    s->issued_ns = now;
    s->effective_ns = 0;
    s->decision_causes = s->causes;
    s->confirmed = 0;
    for (unsigned i = 0; i < 2; ++i) {
        s->applied_ns[i] = 0;
        if (s->applied_pct[i] == target_of(s, i)) {
            s->confirmed |= 1u << i;
            s->applied_ns[i] = now;
        }
    }
    if (s->confirmed == PG_NC_BOTH) s->effective_ns = now;
}

/* Effects we can no longer vouch for. Those workers must confirm again. */
static void mark_unknown(pg_status *s, unsigned workers) {
    s->unknown |= workers;
    for (unsigned i = 0; i < 2; ++i)
        if (workers & (1u << i)) s->applied_pct[i] = PG_CAP_UNKNOWN;
}

/* Enter a restricting state. Caps are reissued only if their level changes. */
static void restrict_to(pg_status *s, unsigned level, uint64_t now) {
    if (level != s->stage) issue(s, level, now);
    s->state = level;
    s->clear_since_ns = 0;
}

/* One tick of an unbroken recovery. NC1 returns after 2.0 s all-clear, NC2
   once that is confirmed and another 1.0 s has passed, and Normal follows the
   confirmation of full caps. */
static void restore_step(pg_policy *p, uint64_t now) {
    pg_status *s = &p->status;
    if (!s->effective_ns) return; /* the previous step has not taken hold */
    if (s->stage == NORMAL) {
        s->state = NORMAL;
        s->clear_since_ns = 0;
        return;
    }
    if (!p->nc1_step_ns) {
        if (now - s->clear_since_ns < PG_RESTORE_NC1) return;
        /* Coming from Throttled, NC1 is already open. NC2 still waits. */
        if (s->nc1_pct == 0) issue(s, THROTTLED, now);
        p->nc1_step_ns = now;
        return;
    }
    if (now - p->nc1_step_ns >= PG_RESTORE_NC2) issue(s, NORMAL, now);
}

/* The supervisor FSM (PRD page 7). required is the strictest level the inputs
   ask for, and all_clear means every input is at or below its clear level. */
static void advance_fsm(pg_policy *p, unsigned required, int all_clear, uint64_t now) {
    pg_status *s = &p->status;

    if (s->state == RECOVERY) {
        /* Anything short of all-clear, or a dwell broken by missing data,
           cancels the restoration. Fall back to the stricter of where it
           began and what the inputs need now, which may close NC1 again. */
        if (!all_clear || !s->clear_since_ns)
            restrict_to(s, required > p->recovery_from ? required : p->recovery_from, now);
        else
            restore_step(p, now);
        return;
    }

    /* A stricter level applies at once, directly from any state. */
    if (required > s->state) {
        restrict_to(s, required, now);
        return;
    }

    /* In the gap above the clear levels, or with the last cap unconfirmed,
       the current state holds and no recovery time accrues. */
    if (!all_clear || !s->effective_ns || s->state == NORMAL) {
        s->clear_since_ns = 0;
        return;
    }

    if (s->state == WARNING) { /* nothing was shed, so no staged restore */
        if (!s->clear_since_ns) s->clear_since_ns = now;
        if (now - s->clear_since_ns >= PG_WARNING_CLEAR) {
            issue(s, NORMAL, now);
            s->state = NORMAL;
            s->clear_since_ns = 0;
        }
        return;
    }

    /* Throttled or Critical: the first all-clear sample starts recovery.
       The caps stay where they are while the dwell runs. */
    p->recovery_from = s->state;
    p->nc1_step_ns = 0;
    s->state = RECOVERY;
    s->clear_since_ns = now;
}

void pg_init(pg_policy *p, uint64_t now) {
    memset(p, 0, sizeof(*p));
    p->status.epoch = now;
    p->status.applied_pct[0] = p->status.applied_pct[1] = PG_CAP_UNKNOWN;
    p->status.state = CRITICAL;
    issue(&p->status, CRITICAL, now);
    pg_tick(p, now);
}

static int fresh(uint64_t stamp, uint64_t now, uint64_t max_age) {
    return stamp != 0 && stamp <= now && now - stamp <= max_age;
}

/* A reading the policy can use: good quality, a known source, in range. */
static int valid_reading(const pg_sample *s) {
    return s->quality == GOOD && (s->source == REAL || s->source == SIMULATED) &&
           s->thermal_seq && s->temp_deci_c >= -400 && s->temp_deci_c <= 1250;
}

void pg_force_off(pg_policy *p) {
    p->forced_off = 1;
}

uint64_t pg_age_deadline(const pg_policy *p) {
    return p->last_good_ns ? p->last_good_ns + PG_TEMP_MAX_AGE : 0;
}

void pg_ingest(pg_policy *p, const pg_sample *s, uint64_t now) {
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
    /* Age only moves forward on a valid reading taken after the last expiry,
       so a record that arrives late cannot undo an expiry already declared. */
    if (!p->bad_sequence && valid_reading(s) && s->acquired_ns > p->last_good_ns &&
        s->acquired_ns > p->expired_at_ns) {
        if (s->verified || !p->last_good_ns || s->temp_deci_c != p->sample.temp_deci_c)
            p->flat_since_ns = s->acquired_ns;
        if (p->status.state == RECOVERY && s->acquired_ns - p->last_good_ns > PG_RECOVERY_GAP)
            p->status.clear_since_ns = 0; /* samples went missing during the dwell */
        p->last_good_ns = s->acquired_ns;
    }
    int bad_load_stamp = s->load_end_ns > now ||
        (p->have_sample && s->load_end_ns < p->sample.load_end_ns);
    p->sample = *s;
    if (bad_load_stamp) p->sample.load_end_ns = 0;
    p->have_sample = 1;
    pg_tick(p, now);
}

/* Only the current decision of this epoch can be confirmed, at its own cap and
   at a time between its issue and now. A repeat is accepted but changes
   nothing, so neither a late nor a duplicate ACK can enable work. */
int pg_worker_effect(pg_policy *p, uint64_t epoch, uint64_t id, unsigned nc,
                     unsigned cap, uint64_t applied, uint64_t now) {
    pg_status *s = &p->status;
    if (nc != 1 && nc != 2) return 0;
    unsigned i = nc - 1, bit = 1u << i;
    if (epoch != s->epoch || id != s->decision_id ||
        cap != target_of(s, i) || !applied || applied < s->issued_ns || applied > now)
        return 0;
    p->manager_seen_ns = now;
    if (s->confirmed & bit) return 1;
    s->confirmed |= bit;
    s->unknown &= ~bit;
    s->applied_ns[i] = applied;
    s->applied_pct[i] = cap;
    if (s->confirmed == PG_NC_BOTH)
        s->effective_ns = s->applied_ns[0] > s->applied_ns[1] ? s->applied_ns[0] : s->applied_ns[1];
    return 1;
}

int pg_effect(pg_policy *p, uint64_t epoch, uint64_t id, uint64_t effective,
              unsigned nc1, unsigned nc2, uint64_t now) {
    pg_status *s = &p->status;
    if (nc1 != s->nc1_pct || nc2 != s->nc2_pct) return 0; /* both or neither */
    return pg_worker_effect(p, epoch, id, 1, nc1, effective, now) &&
           pg_worker_effect(p, epoch, id, 2, nc2, effective, now);
}

void pg_tick(pg_policy *p, uint64_t now) {
    pg_status *s = &p->status;
    const pg_sample *in = &p->sample;
    int peak = 0;
    unsigned required = NORMAL, causes = 0;
    if (now - p->last_eval_ns > PG_CPU_MAX_AGE) {
        s->clear_since_ns = 0;
        p->healthy_since_ns = 0;
    }
    p->last_eval_ns = now;
    s->as_of_ns = now;
    s->lease_until_ns = now + PG_LEASE;
    if (!p->have_sample || p->bad_sequence || !valid_reading(in)) causes |= BAD_TEMP;
    /* The age check uses only acquisition times, so it holds even if the
       sampler stops sending. The QNX service also arms a timer for it. */
    uint64_t deadline = pg_age_deadline(p);
    if (!deadline || now >= deadline) {
        causes |= TEMP_EXPIRED;
        if (deadline > p->expired_at_ns) p->expired_at_ns = deadline;
    }
    /* The same unverified raw value for 2 s may be a stuck sensor. */
    if (p->last_good_ns && !in->verified && p->last_good_ns - p->flat_since_ns >= PG_FLAT_LIMIT)
        causes |= UNVERIFIED_FLAT;
    if (!fresh(in->load_end_ns, now, PG_CPU_MAX_AGE)) causes |= BAD_LOAD;
    for (unsigned i = 0; i < 4; ++i) {
        int busy = in->core_busy_pct[i];
        if (busy < 0 || busy > 100) { causes |= BAD_LOAD; continue; }
        if (busy > peak) peak = busy;
    }
    /* P = 0.4 * NC1 + 0.6 * NC2 from the pre-cap requests (PRD page 3). The
       weights are in tenths, so dividing by 10 rounds down. Requests count
       even while a worker is gated, which keeps P high until demand drops. */
    static const int weight[2] = {4, 6};
    int weighted = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int requested = in->nc_demand_pct[i];
        if (requested < 0 || requested > 100) causes |= BAD_LOAD;
        else weighted += weight[i] * requested;
    }
    int index = weighted / 10;
    s->cpu_peak_pct = (unsigned)peak;
    s->power_index = (unsigned)index;
    unsigned bands[] = {severity(in->temp_deci_c, thermal), severity(peak,cpu),
                        severity(index,power)};
    const unsigned flags[] = {TEMP,CPU,POWER};
    for (unsigned i = 0; i < 3; ++i) {
        if (bands[i]) causes |= flags[i];
        if (bands[i] > required) required = bands[i];
    }
    if (p->forced_off) causes |= WATCHDOG;
    if (causes & (BAD_TEMP | BAD_LOAD | TEMP_EXPIRED | UNVERIFIED_FLAT | WATCHDOG))
        required = CRITICAL;
    /* The manager keeps confirming the target. If it falls silent, what it
       reported can no longer be trusted, and the effect deadline below takes
       over. The current decision is not reissued. */
    if (s->effective_ns && now - p->manager_seen_ns >= PG_LEASE) {
        s->effective_ns = 0;
        s->confirmed = 0;
        mark_unknown(s, PG_NC_BOTH);
    }
    /* Missed effect deadline: the trial fails, and every worker that has not
       confirmed is unknown. Critical still asks both to stop. */
    if (!s->effective_ns && now - s->issued_ns > PG_RESPONSE_TARGET) {
        mark_unknown(s, PG_NC_BOTH & ~s->confirmed);
        causes |= WAIT_EFFECT;
        required = CRITICAL;
    }
    s->causes = causes;
    /* A recovery dwell needs unbroken sampling (PRD page 7). */
    if (s->state == RECOVERY && now - p->last_good_ns > PG_RECOVERY_GAP) s->clear_since_ns = 0;
    if (!pg_recovery_ready(p)) p->healthy_since_ns = 0;
    else if (!p->healthy_since_ns) p->healthy_since_ns = now;
    advance_fsm(p, required, pg_recovery_ready(p), now);
}
