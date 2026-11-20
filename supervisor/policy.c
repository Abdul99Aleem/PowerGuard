/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * supervisor/policy.c
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#include "policy.h"
#include <string.h>

enum cap_stage { FULL = 0, NC2_OFF = 2, BOTH_OFF = 4 };

typedef struct {
    int clear;
    int warn;
    int throttle;
    int critical;
} limits;

static const limits thermal = {.clear = 620, .warn = 650, .throttle = 700, .critical = 750};
static const limits cpu = {.clear = 55, .warn = 70, .throttle = 85, .critical = 95};
static const limits power = {.clear = 45, .warn = 60, .throttle = 75, .critical = 90};

static unsigned severity(int value, limits thresholds)
{
    if (value >= thresholds.critical) {
        return BOTH_OFF;
    }
    if (value >= thresholds.throttle) {
        return NC2_OFF;
    }
    return value >= thresholds.warn ? 1 : 0;
}

int pg_activation_limit(unsigned signal, int value)
{
    const limits bands[] = {thermal, cpu, power};
    if (signal >= 3) {
        return -1;
    }
    const limits thresholds = bands[signal];
    if (value >= thresholds.critical) {
        return thresholds.critical;
    }
    if (value >= thresholds.throttle) {
        return thresholds.throttle;
    }
    return value >= thresholds.warn ? thresholds.warn : -1;
}

int pg_clear_values(int temp, int peak, int demand)
{
    return temp <= thermal.clear && peak <= cpu.clear && demand <= power.clear;
}

int pg_recovery_ready(const pg_policy *p)
{
    return p->status.causes == 0 &&
           pg_clear_values(p->sample.temp_deci_c, p->status.cpu_peak_pct, p->status.power_index);
}

static void set_stage(pg_status *s, unsigned stage, uint64_t now)
{
    s->stage = stage;
    s->nc1_pct = stage == BOTH_OFF ? 0 : 100;
    s->nc2_pct = stage == FULL ? 100 : 0;
    ++s->decision_id;
    s->issued_ns = now;
    s->effective_ns = 0;
    s->clear_since_ns = 0;
}

static void advance_fsm(pg_policy *p, unsigned severity, int clear, uint64_t now)
{
    pg_status *s = &p->status;
    if (!clear) {
        unsigned hold = s->state == RECOVERY ? p->recovery_origin : s->stage;
        if (severity >= NC2_OFF && severity > hold) {
            hold = severity;
        }
        if (hold != s->stage) {
            set_stage(s, hold, now);
        }
        if (hold == BOTH_OFF) {
            s->state = CRITICAL;
        } else if (hold == NC2_OFF) {
            s->state = THROTTLED;
        } else if (severity) {
            s->state = WARNING;
        }
        s->clear_since_ns = 0;
        return;
    }
    if (!s->effective_ns || s->state == NORMAL) {
        return;
    }
    if (s->state == RECOVERY && s->stage == FULL) {
        s->state = NORMAL;
        s->clear_since_ns = 0;
        return;
    }
    if (s->state == CRITICAL || s->state == THROTTLED) {
        p->recovery_origin = s->stage;
        s->state = RECOVERY;
    }
    if (!s->clear_since_ns) {
        s->clear_since_ns = now;
    }
    uint64_t dwell = PG_DWELL;
    if (s->state == RECOVERY && s->stage == NC2_OFF) {
        dwell = p->recovery_origin == BOTH_OFF ? PG_RESTORE_DWELL : PG_DWELL + PG_RESTORE_DWELL;
    }

    uint64_t due = s->clear_since_ns + dwell;
    if (p->sample.acquired_ns < due || p->sample.load_end_ns < due) {
        return;
    }
    if (s->state == WARNING) {
        s->state = NORMAL;
        s->clear_since_ns = 0;
    } else {
        set_stage(s, s->stage == BOTH_OFF ? NC2_OFF : FULL, now);
    }
}

void pg_init(pg_policy *p, uint64_t now)
{
    memset(p, 0, sizeof(*p));
    p->status.epoch = now;
    p->status.state = CRITICAL;
    set_stage(&p->status, BOTH_OFF, now);
    pg_tick(p, now);
}

static int fresh(uint64_t stamp, uint64_t now, uint64_t max_age)
{
    return stamp != 0 && stamp <= now && now - stamp < max_age;
}

static void evaluate(pg_policy *p, uint64_t now, unsigned causes, uint64_t detected, int gap);

static uint64_t earlier(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static void ingest(pg_policy *p, const pg_sample *s, uint64_t now)
{
    if (s->acquired_ns > now) {
        p->bad_sequence = 1;
        pg_tick(p, now);
        return;
    }

    if (p->have_sample &&
        (s->thermal_seq < p->sample.thermal_seq ||
         (s->thermal_seq == p->sample.thermal_seq &&
          (s->acquired_ns != p->sample.acquired_ns || s->temp_deci_c != p->sample.temp_deci_c ||
           s->source != p->sample.source)) ||
         (s->thermal_seq > p->sample.thermal_seq && s->acquired_ns <= p->sample.acquired_ns))) {
        p->bad_sequence = 1;
        p->sample.quality = SENSOR_ERROR;
        pg_tick(p, now);
        return;
    }
    if (!p->have_sample || s->thermal_seq > p->sample.thermal_seq) {
        p->bad_sequence = 0;
    }
    if (s->quality != GOOD && s->quality != READ_ONLY) {
        p->bad_sequence = 1;
    }

    unsigned expired = 0;
    uint64_t detected = now;
    int gap = 0;
    if (p->have_sample) {
        if (!fresh(p->sample.acquired_ns, now, PG_TEMP_MAX_AGE)) {
            expired |= BAD_TEMP;
            detected = earlier(detected, p->sample.acquired_ns + PG_TEMP_MAX_AGE);
        }
        if (!fresh(p->sample.load_end_ns, now, PG_CPU_MAX_AGE)) {
            expired |= BAD_LOAD;
            detected = earlier(detected, p->sample.load_end_ns + PG_CPU_MAX_AGE);
        }
        if (p->sample.quality == READ_ONLY && now - p->flat_since_ns >= PG_FLAT_LIMIT &&
            !(p->status.causes & BAD_TEMP)) {
            expired |= BAD_TEMP;
            detected = earlier(detected, p->flat_since_ns + PG_FLAT_LIMIT);
        }
        gap = (s->acquired_ns > p->sample.acquired_ns + PG_RECOVERY_MAX_GAP ||
               s->load_end_ns > p->sample.load_end_ns + PG_RECOVERY_MAX_GAP);
    }
    if (!p->have_sample || s->quality != READ_ONLY || p->sample.quality != READ_ONLY ||
        s->temp_deci_c != p->sample.temp_deci_c || s->source != p->sample.source) {
        p->flat_since_ns = s->acquired_ns;
    }
    int bad_load_stamp =
        s->load_end_ns > now || (p->have_sample && s->load_end_ns < p->sample.load_end_ns);
    p->sample = *s;
    if (bad_load_stamp) {
        p->sample.load_end_ns = 0;
    }
    p->have_sample = 1;
    evaluate(p, now, expired, detected, gap);
}

void pg_ingest(pg_policy *p, const pg_sample *s, uint64_t now)
{
    p->explicit_power = 0;
    ingest(p, s, now);
}

void pg_ingest_power(pg_policy *p, const pg_sample *s, int index, uint64_t now)
{
    p->explicit_power = 1;
    p->power_value = index;
    ingest(p, s, now);
}

int pg_effect(pg_policy *p, uint64_t epoch, uint64_t id, uint64_t effective, unsigned nc1,
              unsigned nc2, uint64_t now)
{
    pg_status *s = &p->status;
    if (epoch != s->epoch || id != s->decision_id || !effective || effective < s->issued_ns ||
        effective > now || nc1 != s->nc1_pct || nc2 != s->nc2_pct) {
        return 0;
    }
    if (s->effective_ns && s->effective_ns != effective) {
        return 0;
    }
    if (p->effect_received_id != id) {
        p->effect_received_id = id;
        p->effect_received_ns = now;
    }
    p->manager_seen_ns = now;
    s->effective_ns = effective;
    return 1;
}

int pg_manager_progress(pg_policy *p, uint64_t epoch, uint64_t sequence, uint64_t progress_ns,
                        uint64_t now)
{
    if (epoch != p->status.epoch || !sequence || sequence <= p->manager_progress_seq ||
        !fresh(progress_ns, now, PG_MANAGER_MAX_AGE) || progress_ns <= p->manager_progress_ns) {
        return 0;
    }

    if (p->manager_progress_seq) {
        pg_tick(p, now);
    }
    p->manager_progress_seq = sequence;
    p->manager_progress_ns = progress_ns;
    pg_tick(p, now);
    return 1;
}

static void evaluate(pg_policy *p, uint64_t now, unsigned causes, uint64_t detected, int gap)
{
    pg_status *s = &p->status;
    const pg_sample *in = &p->sample;
    int peak = 0;
    unsigned required = 0;
    s->as_of_ns = now;
    s->lease_until_ns = now + PG_LEASE;
    if (!p->have_sample || p->bad_sequence || (in->quality != GOOD && in->quality != READ_ONLY) ||
        (in->quality == READ_ONLY && now - p->flat_since_ns >= PG_FLAT_LIMIT) ||
        (in->source != REAL && in->source != SIMULATED) || !in->thermal_seq ||
        !fresh(in->acquired_ns, now, PG_TEMP_MAX_AGE) || in->temp_deci_c < -400 ||
        in->temp_deci_c > 1250) {
        causes |= BAD_TEMP;
    }
    if (!fresh(in->load_end_ns, now, PG_CPU_MAX_AGE)) {
        causes |= BAD_LOAD;
    }
    for (unsigned i = 0; i < 4; ++i) {
        int busy = in->core_busy_pct[i];
        if (busy < 0 || busy > 100) {
            causes |= BAD_LOAD;
            continue;
        }
        if (busy > peak) {
            peak = busy;
        }
    }
    int weighted_demand = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int requested = in->nc_demand_pct[i];
        if (requested < 0 || requested > 100) {
            causes |= BAD_LOAD;
        } else {
            weighted_demand += (i == 0 ? 2 : 3) * requested;
        }
    }
    s->cpu_peak_pct = (unsigned)peak;

    int index = weighted_demand / 5;
    if (p->explicit_power) {
        if (p->power_value < 0 || p->power_value > 100) {
            causes |= BAD_LOAD;
        } else {
            index = p->power_value;
        }
    }
    s->power_index = (unsigned)index;
    unsigned bands[] = {severity(in->temp_deci_c, thermal), severity(peak, cpu),
                        severity(index, power)};
    const unsigned flags[] = {TEMP, CPU, POWER};
    for (unsigned i = 0; i < 3; ++i) {
        if (bands[i]) {
            causes |= flags[i];
        }
        if (bands[i] > required) {
            required = bands[i];
        }
    }
    if (p->manager_progress_seq && !fresh(p->manager_progress_ns, now, PG_MANAGER_MAX_AGE)) {
        causes |= BAD_MANAGER;
        detected = earlier(detected, p->manager_progress_ns + PG_MANAGER_MAX_AGE);
    }
    if (p->forced_off_ns) {
        causes |= FORCED_OFF;
        detected = earlier(detected, p->forced_off_ns);
    }
    if (causes & (BAD_TEMP | BAD_LOAD | BAD_MANAGER | FORCED_OFF)) {
        required = BOTH_OFF;
    }

    if (p->have_sample) {
        if (bands[0] == required && bands[0] >= NC2_OFF && !(causes & BAD_TEMP)) {
            detected = earlier(detected, in->acquired_ns);
        }
        if ((bands[1] == required || bands[2] == required) && required >= NC2_OFF &&
            !(causes & BAD_LOAD)) {
            detected = earlier(detected, in->load_end_ns);
        }
        if (in->acquired_ns) {
            detected = earlier(detected, in->acquired_ns + PG_TEMP_MAX_AGE);
        }
        if (in->load_end_ns) {
            detected = earlier(detected, in->load_end_ns + PG_CPU_MAX_AGE);
        }
        if (in->quality == READ_ONLY) {
            detected = earlier(detected, p->flat_since_ns + PG_FLAT_LIMIT);
        }
    }
    unsigned previous_stage = s->stage;
    if (s->effective_ns && now - p->manager_seen_ns >= PG_LEASE) {
        set_stage(s, BOTH_OFF, now);
        causes |= WAIT_EFFECT;
        required = BOTH_OFF;
    }
    if (!s->effective_ns && now - s->issued_ns > PG_RESPONSE_TARGET) {
        causes |= WAIT_EFFECT;
        required = BOTH_OFF;
    }
    s->causes = causes;
    int clear = pg_recovery_ready(p) && !gap;
    if (!clear) {
        p->healthy_since_ns = 0;
    } else if (!p->healthy_since_ns) {
        p->healthy_since_ns = now;
    }
    advance_fsm(p, required, clear, now);
    if (s->stage > previous_stage) {
        p->restriction_from_ns = detected;
        p->restriction_decision_id = s->decision_id;
    }
}

void pg_tick(pg_policy *p, uint64_t now)
{
    evaluate(p, now, 0, now, 0);
}

uint64_t pg_next_deadline(const pg_policy *p)
{
    const pg_status *s = &p->status;
    uint64_t manager_deadline =
        s->effective_ns ? p->manager_seen_ns + PG_LEASE : s->issued_ns + PG_RESPONSE_TARGET + 1;
    const uint64_t deadlines[] = {
        manager_deadline, p->manager_progress_seq ? p->manager_progress_ns + PG_MANAGER_MAX_AGE : 0,
        p->have_sample ? p->sample.acquired_ns + PG_TEMP_MAX_AGE : 0,
        p->have_sample ? p->sample.load_end_ns + PG_CPU_MAX_AGE : 0,
        p->sample.quality == READ_ONLY ? p->flat_since_ns + PG_FLAT_LIMIT : 0};
    uint64_t next = 0;
    for (unsigned i = 0; i < sizeof(deadlines) / sizeof(deadlines[0]); ++i) {
        if (deadlines[i] > s->as_of_ns && (!next || deadlines[i] < next)) {
            next = deadlines[i];
        }
    }
    return next;
}
