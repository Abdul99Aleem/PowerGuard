#ifndef PG_THREADCTL_H
#define PG_THREADCTL_H
#include <stdint.h>
typedef struct {
    int32_t pid, tid, cpu, priority, policy, error;
    uint64_t runmask, inherit_mask;
    char name[32];
} pg_thread_report;
typedef struct { uint64_t runmask, inherit_mask; } pg_thread_masks;
int pg_thread_masks_get(pg_thread_masks *out);
int pg_thread_masks_set(uint64_t runmask, uint64_t inherit_mask);
int pg_thread_configure(unsigned cpu, int priority, const char *name, pg_thread_report *out);
int pg_thread_verify(pg_thread_report *out);
unsigned pg_thread_cpu_count(void);
void pg_thread_print(const pg_thread_report *out);
#endif
