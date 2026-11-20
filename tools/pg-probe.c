/* Read-only probe: prints the supervisor's status snapshot every 20 ms.
   It only sends PG_READ, so it is safe to run next to a live system.
   usage: pg-probe [COUNT]   (default 50 lines) */
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>

static long long age_ms(uint64_t now, uint64_t then) {
    return (long long)(now - then) / 1000000;
}

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 50;
    int coid = name_open(PG_NAME, 0);
    if (coid == -1) { perror("name_open PowerGuard"); return 1; }

    for (int i = 0; i < count; ++i) {
        pg_message_v1 query = {.type = PG_READ, .version = PG_VERSION};
        pg_reply_v1 reply;
        if (MsgSend(coid, &query, sizeof(query), &reply, sizeof(reply)) == -1) {
            perror("MsgSend"); return 1;
        }
        const pg_status *s = &reply.decision;
        const pg_sample *in = &reply.last_input;
        long long clear = s->clear_since_ns ? age_ms(s->as_of_ns, s->clear_since_ns) : -1;
        printf("%3d st=%u stage=%u causes=0x%03x id=%llu eff=%d conf=%u unk=%u clear=%lld "
               "seq=%llu acq_age=%lld load_age=%lld T=%d cpu=%u P=%u q=%d ver=%d "
               "cores=%d,%d,%d,%d dem=%d,%d\n",
               i, s->state, s->stage, s->causes, (unsigned long long)s->decision_id,
               s->effective_ns != 0, s->confirmed, s->unknown, clear,
               (unsigned long long)in->thermal_seq,
               age_ms(s->as_of_ns, in->acquired_ns), age_ms(s->as_of_ns, in->load_end_ns),
               in->temp_deci_c, s->cpu_peak_pct, s->power_index, in->quality, in->verified,
               in->core_busy_pct[0], in->core_busy_pct[1], in->core_busy_pct[2],
               in->core_busy_pct[3], in->nc_demand_pct[0], in->nc_demand_pct[1]);
        struct timespec pause = {.tv_nsec = 20 * 1000000};
        nanosleep(&pause, NULL);
    }
    name_close(coid);
    return 0;
}
