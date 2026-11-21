/* Bounded external test disturbance, not a managed application workload. */
#include "pg_threadctl.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/neutrino.h>
#include <time.h>

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { return 0; }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void *heat(void *arg)
{
    unsigned cpu = (unsigned)(uintptr_t)arg;
    pg_thread_report placement;
    if (pg_thread_configure(cpu, 1, "pg-demo-heat", &placement)) { return (void *)1; }
    uint64_t begin = now(), due = begin;
    volatile unsigned value = 1;
    while (due < begin + UINT64_C(20000000000)) {
        /* 30% of each CPU at FIFO1; critical control remains FIFO30. */
        uint64_t until = due + UINT64_C(3000000);
        while (now() < until) { value = value * 1664525u + 1013904223u; }
        due += UINT64_C(10000000);
        uint64_t tolerance = 1;
        if (TimerTimeout(CLOCK_MONOTONIC, TIMER_TOLERANCE, NULL, &tolerance, NULL) == -1) {
            return (void *)1;
        }
        struct timespec release = {due / 1000000000, due % 1000000000};
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &release, NULL)) { return (void *)1; }
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) {
        if (pthread_create(&threads[i], NULL, heat, (void *)(uintptr_t)i)) { return 1; }
    }
    int failed = 0;
    for (unsigned i = 0; i < 4; ++i) {
        void *result;
        if (pthread_join(threads[i], &result) || result) { failed = 1; }
    }
    puts("Bounded test heater finished: 20 seconds, 30% duty per core, FIFO1");
    return failed;
}
