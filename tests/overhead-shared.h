/*
 * POWERGUARD  /  TEAM SCHEDULIX
 * --------------------------------------------------------------------------
 * tests/overhead-shared.h
 *
 * Original software (c) 2026 Abdul Aleem. MIT License.
 * Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.
 */

#ifndef PG_OVERHEAD_SHARED_H
#define PG_OVERHEAD_SHARED_H

#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "ipc.h"
#define SENSOR_PULSE (_PULSE_CODE_MINAVAIL + 1)

typedef struct {
    pthread_mutex_t lock;
    pg_sample sample;
    atomic_uint reads;
    atomic_uint sent;
    atomic_uint received;
    atomic_uint checks;
    atomic_uint trips;
    atomic_uint skipped;
    atomic_int steady;
} overhead_shared;

static overhead_shared *open_overhead(int create)
{
    int fd = open("overhead.bin", create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR, 0600);
    if (fd < 0) {
        return NULL;
    }
    if (create && ftruncate(fd, sizeof(overhead_shared)) != 0) {
        close(fd);
        return NULL;
    }
    overhead_shared *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) {
        return NULL;
    }
    if (create) {
        pthread_mutexattr_t a;
        if (pthread_mutexattr_init(&a) ||
            pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED) ||
            pthread_mutex_init(&s->lock, &a)) {
            return NULL;
        }
        pthread_mutexattr_destroy(&a);
        atomic_init(&s->reads, 0);
        atomic_init(&s->sent, 0);
        atomic_init(&s->received, 0);
        atomic_init(&s->checks, 0);
        atomic_init(&s->trips, 0);
        atomic_init(&s->skipped, 0);
        atomic_init(&s->steady, 0);
    }
    return s;
}

#endif
