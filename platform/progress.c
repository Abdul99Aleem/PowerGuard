#define _POSIX_C_SOURCE 200809L
#include "pg_progress.h"
#include "pg_names.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *progress_name(void) {
    static char name[128];
    return pg_instance_name("/powerguard_progress", name, sizeof(name));
}
pg_progress *pg_progress_open(int create) {
    const char *name = progress_name();
    if (create) {
        /* The caller owns PG_BENCH_NAME: a previous run can no longer admit
         * work. Existing mappings remain attached to the previous object. */
        if (shm_unlink(name) && errno != ENOENT) return NULL;
    }
    int fd = shm_open(name, create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR, 0600);
    if (fd == -1) return NULL;
    struct stat st;
    if ((create && ftruncate(fd, sizeof(pg_progress))) || fstat(fd, &st) ||
        st.st_size != (off_t)sizeof(pg_progress)) {
        int error = errno ? errno : EPROTO;
        close(fd); if (create) shm_unlink(name); errno = error; return NULL;
    }
    pg_progress *p = mmap(NULL, sizeof(*p), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int error = errno;
    close(fd);
    if (p == MAP_FAILED) { if (create) shm_unlink(name); errno = error; return NULL; }
    if (!atomic_is_lock_free(&p->progressed_ns) || !atomic_is_lock_free(&p->forced_ns)) {
        munmap(p, sizeof(*p)); if (create) shm_unlink(name); errno = ENOTSUP; return NULL;
    }
    if (create) {
        atomic_init(&p->version, 0); atomic_init(&p->epoch, 0);
        atomic_init(&p->progressed_ns, 0); atomic_init(&p->forced_ns, 0);
        atomic_store_explicit(&p->version, PG_PROGRESS_VERSION, memory_order_release);
    } else if (atomic_load_explicit(&p->version, memory_order_acquire) != PG_PROGRESS_VERSION) {
        munmap(p, sizeof(*p)); errno = EPROTO; return NULL;
    }
    return p;
}
void pg_progress_close(pg_progress *p, int owner) {
    if (p) munmap(p, sizeof(*p));
    if (owner) shm_unlink(progress_name());
}
