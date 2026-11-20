#include "pg_threadctl.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/neutrino.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static pg_thread_masks child;
static void *inherit_probe(void *unused) {
    (void)unused; CHECK(!pg_thread_masks_get(&child)); return NULL;
}
static atomic_uint counter;
static atomic_int done;
static void *hold_probe(void *unused) {
    (void)unused;
    while (!atomic_load(&done)) {
        atomic_fetch_add(&counter, 1);
        struct timespec pause = {.tv_nsec = 1000000}; nanosleep(&pause, NULL);
    }
    return NULL;
}
static void *exit_probe(void *unused) {
    (void)unused;
    while (!atomic_load(&done)) {
        struct timespec pause = {.tv_nsec = 1000000}; nanosleep(&pause, NULL);
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--inherit-child")) {
        pg_thread_masks actual;
        CHECK(!pg_thread_masks_get(&actual));
        CHECK(actual.runmask == 2 && actual.inherit_mask == 2 && SchedGetCpuNum() == 1);
        return 0;
    }
    pg_thread_masks original;
    CHECK(!pg_thread_masks_get(&original));
    unsigned count = pg_thread_cpu_count();
    CHECK(count >= 4 && count <= 64);
    printf("topology cpus=%u original=0x%" PRIx64 " inherit=0x%" PRIx64 "\n",
           count, original.runmask, original.inherit_mask);
    for (unsigned cpu = 0; cpu < count; ++cpu) {
        pg_thread_report r;
        CHECK(!pg_thread_configure(cpu, 10, "pg-affinity-test", &r));
        pg_thread_print(&r);
        CHECK(r.runmask == (UINT64_C(1) << cpu));
        for (unsigned i = 0; i < 10000; ++i) CHECK(SchedGetCpuNum() == cpu);
    }
    /* A child inherits inherit_mask, not the creator's current runmask. */
    CHECK(!pg_thread_masks_set(1, 2));
    pthread_t t;
    CHECK(!pthread_create(&t, NULL, inherit_probe, NULL)); CHECK(!pthread_join(t, NULL));
    CHECK(child.runmask == 2 && child.inherit_mask == 2);
    puts("PASS child inherits CPU1 while parent runs on CPU0");
    char path[1024]; CHECK(realpath(argv[0], path) != NULL);
    char *args[] = {path, "--inherit-child", NULL};
    pid_t pid; int status;
    CHECK(!posix_spawn(&pid, path, NULL, NULL, args, environ));
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("PASS spawned process inherits CPU1 while parent runs on CPU0");
    uint64_t swapped = 4;
    CHECK(ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_64, &swapped) != -1 && swapped == 1);
    CHECK(ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_64, &swapped) != -1 && swapped == 4);
    puts("PASS 64-bit runmask exchange and restoration");
    CHECK(ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)8) != -1);
    pg_thread_masks actual;
    CHECK(!pg_thread_masks_get(&actual) && actual.runmask == 8 && actual.inherit_mask == 2);
    CHECK(!pg_thread_masks_set(1, 2));
    puts("PASS direct runmask changes leave inherit mask unchanged");
    errno = 0;
    CHECK(pg_thread_masks_set(0, 1) == -1 && errno == EINVAL);
    if (count < 64) {
        uint64_t bad = UINT64_C(1) << count;
        CHECK(ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_64, &bad) == -1 && errno == EINVAL);
    }
    CHECK(!pg_thread_masks_get(&actual) && actual.runmask == 1 && actual.inherit_mask == 2);
    puts("PASS invalid-mask rejection");
    /* Exercise hold/continue only on this disposable lock-free test thread. */
    CHECK(!pthread_create(&t, NULL, hold_probe, NULL));
    struct timespec pause = {.tv_nsec = 20000000}; nanosleep(&pause, NULL);
    CHECK(ThreadCtl(_NTO_TCTL_ONE_THREAD_HOLD, (void *)(uintptr_t)t) != -1);
    unsigned before = atomic_load(&counter);
    nanosleep(&pause, NULL); CHECK(atomic_load(&counter) == before);
    CHECK(ThreadCtl(_NTO_TCTL_ONE_THREAD_CONT, (void *)(uintptr_t)t) != -1);
    nanosleep(&pause, NULL); CHECK(atomic_load(&counter) > before);
    atomic_store(&done, 1); CHECK(!pthread_join(t, NULL));
    puts("PASS isolated thread hold/continue (not used for production caps)");
    int chid = ChannelCreate(0);
    CHECK(chid != -1);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    CHECK(coid != -1);
    struct sigevent event;
    SIGEV_PULSE_INIT(&event, coid, 10, _PULSE_CODE_MINAVAIL, 77);
    atomic_store(&done, 0);
    CHECK(!pthread_create(&t, NULL, exit_probe, NULL));
    CHECK(ThreadCtlExt(0, (int)t, _NTO_TCTL_ADD_EXIT_EVENT, &event) != -1);
    atomic_store(&done, 1);
    uint64_t timeout = UINT64_C(500000000);
    CHECK(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout, NULL) != -1);
    struct _pulse pulse;
    CHECK(MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) == 0);
    CHECK(pulse.code == _PULSE_CODE_MINAVAIL && pulse.value.sival_int == 77);
    CHECK(!pthread_join(t, NULL));
    puts("PASS local thread-exit pulse notification");
    atomic_store(&done, 0);
    CHECK(!pthread_create(&t, NULL, exit_probe, NULL));
    CHECK(ThreadCtlExt(0, (int)t, _NTO_TCTL_ADD_EXIT_EVENT, &event) != -1);
    CHECK(ThreadCtlExt(0, (int)t, _NTO_TCTL_DEL_EXIT_EVENT, NULL) != -1);
    atomic_store(&done, 1); CHECK(!pthread_join(t, NULL));
    timeout = UINT64_C(50000000);
    CHECK(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout, NULL) != -1);
    CHECK(MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) == -1 && errno == ETIMEDOUT);
    CHECK(ConnectDetach(coid) != -1 && ChannelDestroy(chid) != -1);
    puts("PASS deleted thread-exit notification stays silent");
    CHECK(!pg_thread_masks_set(original.runmask, original.inherit_mask));
    puts("PASS restored original runmask and inherit mask");
    return 0;
}
