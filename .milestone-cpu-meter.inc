/* On-board per-core meter. These readings come from QNX idle-thread clocks. */
#include <devctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/procfs.h>
#include <sys/syspage.h>

typedef struct {
    uint64_t end_ns, idle_ns[4];
} cpu_sample;

static clockid_t idle_clock[4];
static cpu_sample history[3];
static unsigned sample_count;
static struct {
    uint64_t epoch, decision, measured_ns, window_ns, issued_ns;
    int32_t busy[4];
} spike_record;
static uint64_t cpu_window_ns;

static uint64_t clock_ns(clockid_t id) {
    struct timespec t;
    if (clock_gettime(id, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static int find_idle_clocks(void) {
    if (_syspage_ptr->num_cpu != 4) {
        fputs("CPU meter expects four cores on the Pi 4.\n", stderr);
        return 0;
    }
    int fd = open("/proc/1/as", O_RDONLY);
    if (fd < 0) { perror("open procnto status"); return 0; }
    unsigned found = 0;
    for (unsigned tid = 1; tid <= 128 && found != 15;) {
        procfs_status status = {.tid = tid};
        int error = devctl(fd, DCMD_PROC_TIDSTATUS, &status, sizeof status, NULL);
        if (error) {
            if (error != ESRCH) { errno = error; perror("read procnto thread status"); }
            break;
        }
        tid = (unsigned)status.tid + 1;
        if (status.real_priority > 1 || status.last_cpu >= 4) continue;
        unsigned core = status.last_cpu;
        struct { int new_name_len, name_buf_len; char name_buf[64]; } label = {-1,64,{0}};
        char expected[64]; snprintf(expected,sizeof expected,"idle_cpu_%u",core);
        if (ThreadCtlExt(1,status.tid,_NTO_TCTL_NAME,&label)<0) continue;
        label.name_buf[sizeof label.name_buf-1]=0;
        if (strcmp(label.name_buf,expected) || (found & (1u<<core))) continue;
        int id = ClockId(1, (int)status.tid);
        if (id == -1) { perror("ClockId idle thread"); break; }
        idle_clock[core] = (clockid_t)id;
        found |= 1u << core;
        printf("Per-core meter: CPU%u uses procnto idle TID %u.\n",core,(unsigned)status.tid);
    }
    close(fd);
    if (found != 15) fputs("Per-core idle clocks unavailable.\n",stderr);
    return found == 15;
}

static int read_core_load(pg_sample *sample) {
    cpu_sample current;
    for (unsigned i=0;i<4;++i) {
        current.idle_ns[i]=clock_ns(idle_clock[i]);
        if (!current.idle_ns[i]) return -1;
    }
    current.end_ns=clock_ns(CLOCK_MONOTONIC);
    unsigned slot=sample_count % 3;
    cpu_sample earlier=history[slot];
    history[slot]=current;
    ++sample_count;
    if (sample_count<=3) return 0;
    if (current.end_ns<=earlier.end_ns) return -1;
    cpu_window_ns=current.end_ns-earlier.end_ns;
    for (unsigned i=0;i<4;++i) {
        if (current.idle_ns[i]<earlier.idle_ns[i]) return -1;
        uint64_t idle=current.idle_ns[i]-earlier.idle_ns[i];
        sample->core_busy_pct[i]=idle>=cpu_window_ns ? 0 :
            (int32_t)((cpu_window_ns-idle)*100/cpu_window_ns);
    }
    sample->load_end_ns=current.end_ns;
    return 1;
}

static void record_spike(const pg_status *decision, const pg_sample *sample) {
    if (spike_record.measured_ns || decision->state!=THROTTLED || !(decision->causes & CPU) ||
        sample->core_busy_pct[3]<85) return;
    spike_record.epoch=decision->epoch;
    spike_record.decision=decision->decision_id;
    spike_record.measured_ns=sample->load_end_ns;
    spike_record.window_ns=cpu_window_ns;
    spike_record.issued_ns=decision->issued_ns;
    memcpy(spike_record.busy,sample->core_busy_pct,sizeof spike_record.busy);
}

static void export_spike(void) {
    if (!spike_record.measured_ns) return;
    FILE *f=fopen("real-cpu-spike.csv","w");
    if (!f) { perror("real-cpu-spike.csv"); return; }
    fprintf(f,"%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%d\n",
            spike_record.epoch,spike_record.decision,spike_record.measured_ns,spike_record.window_ns,
            spike_record.issued_ns,spike_record.busy[0],spike_record.busy[1],spike_record.busy[2],spike_record.busy[3]);
    if (fclose(f)) perror("close real-cpu-spike.csv");
}
