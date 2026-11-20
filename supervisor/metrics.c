#include "metrics.h"

static uint64_t max_time(uint64_t a, uint64_t b) { return a > b ? a : b; }

int pg_trial(pg_metrics *m, unsigned series, unsigned from, unsigned to,
             uint64_t start, uint64_t now) {
    if (!series || series > PG_CASES || from > RECOVERY || to > RECOVERY ||
        from == to || !start || start > now) return 0;
    if (m->trial_ns) ++m->scores.cancelled_trials;
    m->trial_ns = start;
    m->trial_case = series - 1;
    m->trial_from = from;
    m->trial_to = to;
    return 1;
}

int pg_job(pg_metrics *m, uint64_t id, uint64_t release, uint64_t deadline,
           uint64_t finish, uint64_t now) {
    pg_scores *r = &m->scores;
    if (!id || id <= r->last_job_id || !release || release > now || !deadline ||
        (finish && (finish < release || finish > now)) ||
        (!finish && now - release <= deadline)) return 0;
    r->job_gaps += id - r->last_job_id - 1;
    r->last_job_id = id;
    ++r->jobs;
    r->last_finished = finish != 0;
    r->last_latency_ns = finish ? finish - release : 0;
    if (!finish || r->last_latency_ns > deadline) ++r->misses;
    r->max_latency_ns = max_time(r->max_latency_ns, r->last_latency_ns);
    return 1;
}

void pg_observe(pg_metrics *m, const pg_status *old, const pg_policy *p) {
    const pg_status *s = &p->status;
    pg_scores *r = &m->scores;
    uint64_t now = s->as_of_ns;
    if (m->trial_ns && s->state != old->state) {
        if (old->state == m->trial_from && s->state == m->trial_to) {
            pg_spread *b = &m->trials[m->trial_case][old->state][s->state];
            uint64_t elapsed = now - m->trial_ns;
            if (!b->count || elapsed < b->min_ns) b->min_ns = elapsed;
            b->max_ns = max_time(b->max_ns, elapsed);
            ++b->count;
        } else ++r->cancelled_trials;
        m->trial_ns = 0;
    }
    if (!m->started_normal) {
        if (s->state == NORMAL) m->started_normal = 1;
        return; /* exclude safe startup from experiment metrics */
    }

    int values[] = {p->sample.temp_deci_c,(int)s->cpu_peak_pct,(int)s->power_index};
    int valid[] = {!(s->causes & BAD_TEMP), !(s->causes & BAD_LOAD), !(s->causes & BAD_LOAD)};
    if (s->decision_id != old->decision_id) {
        if (r->pending_id) { ++r->unconfirmed; r->pending_id = 0; }
        if (s->stage > old->stage) {
            ++r->issued;
            r->pending_id = s->decision_id;
            m->pending_ns = p->restriction_from_ns ? p->restriction_from_ns : s->issued_ns;
            for (unsigned i=0; i<3; ++i) {
                int limit = pg_activation_limit(i, values[i]);
                if (limit < 0 || !valid[i]) continue;
                /* If response windows overlap, retain the lower crossed limit
                   in its own window. Each band is stored independently. */
                unsigned slot = 3;
                for (unsigned j=0; j<3; ++j) {
                    if (m->windows[i][j].limit == limit) { slot=j; break; }
                    if (m->windows[i][j].until_ns <= now) slot=j;
                }
                if (slot < 3) {
                    m->windows[i][slot].limit = limit;
                    m->windows[i][slot].until_ns = now + 100 * PG_MS;
                }
            }
        }
    }
    for (unsigned i=0; i<3; ++i) {
        if (!valid[i]) continue;
        for (unsigned band=0; band<3; ++band) {
            if (!m->windows[i][band].until_ns || now > m->windows[i][band].until_ns) continue;
            int excess = values[i] - m->windows[i][band].limit;
            if (excess > r->overshoot[i]) r->overshoot[i] = excess;
            r->observed_mask |= 1u << i;
        }
    }
    if (r->pending_id) {
        if (s->effective_ns && s->decision_id == r->pending_id) {
            uint64_t latency = s->effective_ns - m->pending_ns;
            ++r->completed;
            if (latency > PG_RESPONSE_TARGET) ++r->late;
            r->worst_response_ns = max_time(r->worst_response_ns,latency);
            r->pending_id = 0;
        }
    }
    if (s->stage == 0 && (s->state == RECOVERY || old->state == RECOVERY) && s->effective_ns &&
        p->healthy_since_ns && s->effective_ns >= p->healthy_since_ns &&
        s->decision_id != m->last_restore_id) {
        m->last_restore_id = s->decision_id;
        ++r->restores;
        r->last_restore_ns = s->effective_ns - p->healthy_since_ns;
        r->max_restore_ns = max_time(r->max_restore_ns, r->last_restore_ns);
    }
}
