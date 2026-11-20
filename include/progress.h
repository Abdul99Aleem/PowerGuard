#ifndef POWERGUARD_PROGRESS_H
#define POWERGUARD_PROGRESS_H
#include "policy.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Shared between the supervisor, the watchdog and the manager (PRD page 2).
   Every field has a single writer and is read with atomic loads, so no
   process ever waits on another. */
#define PG_PROGRESS_SHM "/powerguard_progress"
#define PG_WATCHDOG_PERIOD (50 * PG_MS)
#define PG_WATCHDOG_STALL (200 * PG_MS)

typedef struct {
    /* written by the supervisor */
    uint64_t epoch;
    uint64_t progress;      /* advances after every completed evaluation */
    uint64_t progress_ns;   /* when it last advanced */
    /* written by the watchdog */
    uint64_t forced_off_ns; /* set: both workers stay off for the rest of this run */
    uint64_t stalled_at_ns; /* the last progress it saw before latching */
} pg_progress;

/* Writers create the record; readers get NULL until it exists. */
static inline pg_progress *pg_progress_open(int writer) {
    int fd = shm_open(PG_PROGRESS_SHM, writer ? O_RDWR | O_CREAT : O_RDONLY, 0644);
    if (fd == -1) return NULL;
    if (writer && ftruncate(fd, sizeof(pg_progress)) == -1) { close(fd); return NULL; }
    void *m = mmap(NULL, sizeof(pg_progress), writer ? PROT_READ | PROT_WRITE : PROT_READ,
                   MAP_SHARED, fd, 0);
    close(fd);
    return m == MAP_FAILED ? NULL : m;
}

static inline uint64_t pg_load(const uint64_t *field) {
    return __atomic_load_n(field, __ATOMIC_ACQUIRE);
}

static inline void pg_store(uint64_t *field, uint64_t value) {
    __atomic_store_n(field, value, __ATOMIC_RELEASE);
}

/* The record outlives a run, so a latch only counts for the supervisor run
   it was raised in. A new supervisor epoch is a new run. */
static inline int pg_forced_off(const pg_progress *p) {
    uint64_t forced = pg_load(&p->forced_off_ns);
    return forced && forced >= pg_load(&p->epoch);
}
#endif
