#include "policy.h"
#include <assert.h>
#include <stdio.h>

static void input(pg_policy *p, uint64_t n, int temp)
{
    pg_sample s = {.thermal_seq = n, .acquired_ns = n, .load_end_ns = n,
                   .temp_deci_c = temp, .quality = GOOD, .source = SIMULATED};
    pg_ingest(p, &s, n);
    assert(pg_effect(p, p->status.epoch, p->status.decision_id,
                     p->status.effective_ns ? p->status.effective_ns : n,
                     p->status.nc1_pct, p->status.nc2_pct, n));
}

int main(void)
{
    assert(PG_THERMAL_CLEAR_DECI == 560);
    assert(pg_activation_limit(0, 565) == 565);
    assert(pg_activation_limit(0, 570) == 570);
    assert(pg_activation_limit(0, 575) == 575);
    pg_policy p;
    uint64_t n = 1000 * PG_MS;
    pg_init(&p, n);
    for (unsigned i = 0; i < 140; ++i) { n += 25 * PG_MS; input(&p, n, 555); }
    assert(p.status.state == NORMAL);
    n += 25 * PG_MS; input(&p, n, 565);
    assert(p.status.state == WARNING && p.status.causes == TEMP);
    n += 25 * PG_MS; input(&p, n, 570);
    assert(p.status.state == THROTTLED && p.status.nc1_pct == 100 && !p.status.nc2_pct);
    n += 25 * PG_MS; input(&p, n, 575);
    assert(p.status.state == CRITICAL && !p.status.nc1_pct && !p.status.nc2_pct);
    for (unsigned i = 0; i < 140; ++i) { n += 25 * PG_MS; input(&p, n, 555); }
    assert(p.status.state == NORMAL && p.status.nc1_pct == 100 && p.status.nc2_pct == 100);
    puts("PASS labelled low-temperature demo: 56.5/57.0/57.5 C activation, 56 C recovery, ordered caps and dwell");
    return 0;
}
