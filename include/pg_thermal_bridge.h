#ifndef PG_THERMAL_BRIDGE_H
#define PG_THERMAL_BRIDGE_H
#include "policy.h"
#include "pgbench_protocol.h"
#include <errno.h>

/* Preserve acquisition identity. Receiving/replaying a sample never renews it. */
static inline int pg_thermal_accept(pg_status_t *s, const pg_thermal_request *r, uint64_t now)
{
    if (!(s->features & PG_FEATURE_REAL_THERMAL)) {
        return ENOTSUP;
    }
    if (r->quality != READ_ONLY && r->quality != SENSOR_ERROR) {
        return EINVAL;
    }
    if (r->temperature_deci_c < -400 || r->temperature_deci_c > 1250 || r->acquired_ns > now ||
        r->acquired_ns < s->thermal_acquired_ns || (r->quality == READ_ONLY && !r->acquired_ns) ||
        (r->acquired_ns && r->acquired_ns == s->thermal_acquired_ns &&
         r->temperature_deci_c * 100 != s->temperature_milli_c)) {
        return EINVAL;
    }
    s->thermal_acquired_ns = r->acquired_ns;
    s->temperature_milli_c = r->temperature_deci_c * 100;
    if (r->quality == SENSOR_ERROR) {
        s->features |= PG_FEATURE_THERMAL_ERROR;
    } else {
        s->features &= ~PG_FEATURE_THERMAL_ERROR;
    }
    return 0;
}
#endif
