#include "pg_thermal_bridge.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void deliver(pg_policy *p, pg_status_t *s, uint64_t now, int acquire)
{
    if (acquire) {
        pg_thermal_request r = {.acquired_ns = now,
                                .temperature_deci_c = 450 + (int)((now / PG_MS / 25) % 2),
                                .quality = READ_ONLY};
        assert(pg_thermal_accept(s, &r, now) == 0);
    }
    pg_sample sample = {.thermal_seq = s->thermal_acquired_ns,
                        .acquired_ns = s->thermal_acquired_ns,
                        .load_end_ns = now,
                        .temp_deci_c = s->temperature_milli_c / 100,
                        .source = REAL,
                        .quality = READ_ONLY};
    pg_ingest(p, &sample, now);
    assert(pg_effect(p, p->status.epoch, p->status.decision_id,
                     p->status.effective_ns ? p->status.effective_ns : now, p->status.nc1_pct,
                     p->status.nc2_pct, now));
}

int main(void)
{
    pg_status_t s = {0};
    pg_thermal_request r = {
        .acquired_ns = 100 * PG_MS, .temperature_deci_c = 450, .quality = READ_ONLY};
    assert(pg_thermal_accept(&s, &r, 101 * PG_MS) == ENOTSUP);
    s.features = PG_FEATURE_REAL_THERMAL | PG_FEATURE_THERMAL_ERROR;
    assert(pg_thermal_accept(&s, &r, 101 * PG_MS) == 0);
    assert(s.temperature_milli_c == 45000 && s.thermal_acquired_ns == 100 * PG_MS);
    assert(!(s.features & PG_FEATURE_THERMAL_ERROR));
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == 0);
    assert(s.thermal_acquired_ns == 100 * PG_MS); /* Replay cannot renew age. */
    r.temperature_deci_c = 460;
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == EINVAL);
    r.acquired_ns = 401 * PG_MS;
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == EINVAL);
    r.acquired_ns = 99 * PG_MS;
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == EINVAL);
    r.acquired_ns = 125 * PG_MS;
    r.temperature_deci_c = INT_MAX;
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == EINVAL);
    r.temperature_deci_c = 460;
    r.quality = GOOD;
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == EINVAL);
    r.quality = SENSOR_ERROR;
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == 0);
    assert(s.features & PG_FEATURE_THERMAL_ERROR);
    r.quality = READ_ONLY;
    r.acquired_ns = 150 * PG_MS;
    assert(pg_thermal_accept(&s, &r, 400 * PG_MS) == 0);
    assert(!(s.features & PG_FEATURE_THERMAL_ERROR));

    /* Feed the bridge's timestamps into the actual policy, including expiry. */
    pg_policy p;
    uint64_t n = 1000 * PG_MS;
    pg_init(&p, n);
    pg_sample sample = {.thermal_seq = s.thermal_acquired_ns,
                        .acquired_ns = s.thermal_acquired_ns,
                        .load_end_ns = n,
                        .temp_deci_c = s.temperature_milli_c / 100,
                        .source = REAL,
                        .quality = READ_ONLY};
    pg_ingest(&p, &sample, n);
    assert(p.status.state == CRITICAL && (p.status.causes & BAD_TEMP));
    n = 1000 * PG_MS;
    pg_init(&p, n);
    s = (pg_status_t){.features = PG_FEATURE_REAL_THERMAL};
    for (unsigned i = 0; i < 140; ++i) {
        n += 25 * PG_MS;
        deliver(&p, &s, n, 1);
    }
    assert(p.status.state == NORMAL);
    for (unsigned i = 0; i < 7; ++i) {
        n += 25 * PG_MS;
        deliver(&p, &s, n, 0);
    }
    assert(p.status.state == CRITICAL && (p.status.causes & BAD_TEMP));
    assert(!p.status.nc1_pct && !p.status.nc2_pct);
    uint64_t resumed = n + 25 * PG_MS;
    for (unsigned i = 0; i < 140; ++i) {
        n += 25 * PG_MS;
        deliver(&p, &s, n, 1);
        if (n < resumed + PG_DWELL) {
            assert(!p.status.nc1_pct);
        }
        if (n < resumed + PG_DWELL + PG_RESTORE_DWELL) {
            assert(!p.status.nc2_pct);
        }
    }
    assert(p.status.state == NORMAL && p.status.nc1_pct == 100 && p.status.nc2_pct == 100);
    puts(
        "PASS real thermal bridge: source isolation, replay/future/range rejection, error and stale fail-safe");
    return 0;
}
