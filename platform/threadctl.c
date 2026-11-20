#include "pg_threadctl.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <unistd.h>

unsigned pg_thread_cpu_count(void) { return _syspage_ptr->num_cpu; }

/* The size field counts unsigned words, not bytes. The two masks follow it.
 * Use the actual topology size, even when the intended mask contains one CPU.
 */
static int masks(uint64_t run, uint64_t inherit, pg_thread_masks *previous) {
    unsigned cpus = pg_thread_cpu_count();
    if (!cpus || cpus > 64 || (cpus < 64 && ((run | inherit) >> cpus))) {
        errno = EINVAL; return -1;
    }
    int words = RMSK_SIZE(cpus);
    struct { int size; unsigned masks[4]; } storage = {0};
    int *data = &storage.size;
    *data = words;
    unsigned *r = (unsigned *)(data + 1), *i = r + words;
    for (unsigned cpu = 0; cpu < cpus; ++cpu) {
        if (run & (UINT64_C(1) << cpu)) RMSK_SET(cpu, r);
        if (inherit & (UINT64_C(1) << cpu)) RMSK_SET(cpu, i);
    }
    int result = ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, data);
    int saved = errno;
    if (result != -1 && previous) {
        *previous = (pg_thread_masks){0};
        for (unsigned cpu = 0; cpu < cpus; ++cpu) {
            if (RMSK_ISSET(cpu, r)) previous->runmask |= UINT64_C(1) << cpu;
            if (RMSK_ISSET(cpu, i)) previous->inherit_mask |= UINT64_C(1) << cpu;
        }
    }
    errno = saved;
    return result == -1 ? -1 : 0;
}
int pg_thread_masks_get(pg_thread_masks *out) {
    if (!out) { errno = EINVAL; return -1; }
    return masks(0, 0, out);
}
int pg_thread_masks_set(uint64_t run, uint64_t inherit) {
    if (!run || !inherit) { errno = EINVAL; return -1; }
    if (masks(run, inherit, NULL)) return -1;
    pg_thread_masks actual;
    if (pg_thread_masks_get(&actual)) return -1;
    if (actual.runmask != run || actual.inherit_mask != inherit) { errno = EIO; return -1; }
    return 0;
}
static int thread_name(const char *name, char *out, size_t capacity) {
    struct { int new_name_len, name_buf_len; char name_buf[64]; } value = {0};
    if (name && strlen(name) >= sizeof(value.name_buf)) { errno = EINVAL; return -1; }
    value.name_buf_len = sizeof(value.name_buf);
    value.new_name_len = name ? (int)strlen(name) : -1;
    if (name) memcpy(value.name_buf, name, strlen(name));
    if (ThreadCtl(_NTO_TCTL_NAME, &value) == -1) return -1;
    value.name_buf[sizeof(value.name_buf)-1] = 0;
    if (!name && out && capacity) {
        size_t count = strlen(value.name_buf);
        if (count >= capacity) { errno = EOVERFLOW; return -1; }
        memcpy(out, value.name_buf, count + 1);
    }
    return 0;
}
int pg_thread_verify(pg_thread_report *out) {
    pg_thread_masks actual;
    uint64_t run64 = 0;
    struct sched_param priority;
    if (pg_thread_masks_get(&actual) ||
        ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_64, &run64) == -1) return -1;
    if (run64 != actual.runmask) { errno = EIO; return -1; }
    int e = pthread_getschedparam(pthread_self(), &out->policy, &priority);
    if (e) { errno = e; return -1; }
    out->pid = getpid(); out->tid = gettid(); out->cpu = SchedGetCpuNum();
    out->priority = priority.sched_priority;
    out->runmask = run64; out->inherit_mask = actual.inherit_mask;
    out->error = 0;
    return thread_name(NULL, out->name, sizeof(out->name));
}
int pg_thread_configure(unsigned cpu, int priority, const char *name, pg_thread_report *out) {
    if (!out || cpu >= pg_thread_cpu_count() || cpu >= 64 ||
        priority < sched_get_priority_min(SCHED_FIFO) ||
        priority > sched_get_priority_max(SCHED_FIFO)) { errno = EINVAL; return -1; }
    uint64_t bit = UINT64_C(1) << cpu;
    struct sched_param param = {.sched_priority = priority};
    int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (e) { errno = e; return -1; }
    if (pg_thread_masks_set(bit, bit) || thread_name(name, NULL, 0) || pg_thread_verify(out)) return -1;
    if (out->runmask != bit || out->inherit_mask != bit || out->cpu != (int)cpu ||
        out->priority != priority || out->policy != SCHED_FIFO || strcmp(out->name, name)) {
        errno = EIO; return -1;
    }
    return 0;
}
void pg_thread_print(const pg_thread_report *r) {
    printf("READY name=%s pid=%d tid=%d cpu=%d runmask=0x%" PRIx64
           " inherit=0x%" PRIx64 " policy=%d priority=%d\n",
           r->name, r->pid, r->tid, r->cpu, r->runmask, r->inherit_mask, r->policy, r->priority);
    fflush(stdout);
}
