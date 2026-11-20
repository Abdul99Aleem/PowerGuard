#ifndef PG_PROGRESS_H
#define PG_PROGRESS_H
#include <stdint.h>
#include <stdatomic.h>

#define PG_PROGRESS_VERSION 1u
#define PG_PROGRESS_LIMIT UINT64_C(200000000)
/* Separate from each server's timer pulse. Pulses never wait for its reply. */
#define PG_PULSE_WATCHDOG (_PULSE_CODE_MINAVAIL + 2)
#define PG_PULSE_FORCE_OFF (_PULSE_CODE_MINAVAIL + 3)
typedef struct {
    atomic_uint version;
    _Atomic uint64_t epoch, progressed_ns, forced_ns;
} pg_progress;

/* Creation is allowed only after acquiring the unique workload channel. The
 * workload owner initializes a new run; no live process can clear forced_ns. */
pg_progress *pg_progress_open(int create);
void pg_progress_close(pg_progress *p, int owner);
static inline void pg_progress_publish(pg_progress *p, uint64_t epoch, uint64_t now) {
    atomic_store_explicit(&p->epoch, epoch, memory_order_relaxed);
    atomic_store_explicit(&p->progressed_ns, now, memory_order_release);
}
static inline int pg_progress_stale(const pg_progress *p, uint64_t now) {
    uint64_t stamp = atomic_load_explicit(&p->progressed_ns, memory_order_acquire);
    return stamp && now >= stamp && now - stamp >= PG_PROGRESS_LIMIT;
}
static inline uint64_t pg_progress_forced(const pg_progress *p) {
    return p ? atomic_load_explicit(&p->forced_ns, memory_order_acquire) : 0;
}
static inline int pg_progress_allows(const pg_progress *p, unsigned nc1, unsigned nc2) {
    return !pg_progress_forced(p) || (!nc1 && !nc2);
}
static inline void pg_progress_trip(pg_progress *p, uint64_t now) {
    uint64_t empty = 0;
    (void)atomic_compare_exchange_strong_explicit(&p->forced_ns, &empty, now,
        memory_order_release, memory_order_relaxed);
}
#endif
