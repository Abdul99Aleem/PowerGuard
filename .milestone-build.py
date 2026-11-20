from pathlib import Path
import subprocess
import sys

root = Path.cwd()
overhead = sys.argv[1:] == ['--overhead']
out = root / 'build' / ('overhead' if overhead else 'milestone')
out.mkdir(exist_ok=True)
def qcc(*args):
    subprocess.run(['qcc','-Vgcc_ntoaarch64le',*map(str,args)],check=True)

def write_assets(header, assets):
    with header.open('w') as f:
        for label,path in assets:
            contents = path.read_bytes()
            f.write('static const unsigned char '+label+'[]={\n')
            for pos in range(0,len(contents),32):
                f.write(','.join(map(str,contents[pos:pos+32]))+',\n')
            f.write('};\n')

if sys.argv[1:] == ['--ham-suite']:
    flags = ['-Wall','-Wextra','-Werror','-std=gnu11','-O2','-Iinclude','-Itestbench','-I'+str(out)]
    trials = [('throttled',[]),('critical',['-DCRITICAL_TRIAL']),
              ('recovery',['-DCRITICAL_TRIAL','-DRECOVERY_TRIAL'])]
    for label,defines in trials:
        qcc(*flags,'-DHAM_ON_TRIAL',*defines,root/'.milestone-runner.c','-o',out/('ham-'+label))
    write_assets(out/'ham-suite-assets.h',[(label,out/('ham-'+label)) for label,_ in trials])
    qcc(*flags,root/'.milestone-ham-suite.c','-o',out/'pg-t3-ham-suite')
    sys.exit(0)

bench = (root / 'testbench/pgbench.c').read_text()
bench = bench.replace('static void *critical_worker(void *unused) {', r'''static struct { uint64_t release, start, finish; } milestone_jobs[10000];
static size_t milestone_count;
static unsigned milestone_dropped;
static void record_job(uint64_t release, uint64_t start, uint64_t finish) {
    if (milestone_count == sizeof milestone_jobs / sizeof milestone_jobs[0]) {
        ++milestone_dropped;
        return;
    }
    size_t i = milestone_count++;
    milestone_jobs[i].release = release;
    milestone_jobs[i].start = start;
    milestone_jobs[i].finish = finish;
}
static void export_jobs(void) {
    FILE *f = fopen("critical-jobs.csv", "w");
    if (!f) { perror("critical-jobs.csv"); return; }
    fputs("release_ns,start_ns,finish_ns\n", f);
    for (size_t i = 0; i < milestone_count; ++i)
        fprintf(f, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
          milestone_jobs[i].release, milestone_jobs[i].start, milestone_jobs[i].finish);
    if (fclose(f)) perror("critical-jobs.csv close");
    printf("Critical local records=%zu buffer_overflow=%u\n", milestone_count, milestone_dropped);
}
static void *critical_worker(void *unused) {''',1)
bench = bench.replace('uint64_t response = ended > release', 'record_job(release, began, ended);\n        uint64_t response = ended > release',1)
bench = bench.replace('st.critical_jobs += skipped;', 'for (uint64_t j = 0; j < skipped; ++j) record_job(release + j * PERIOD_NS,0,0);\n            st.critical_jobs += skipped;',1)
bench = bench.replace('pthread_join(sample, NULL); pthread_join(logger, NULL);', 'pthread_join(sample, NULL); pthread_join(logger, NULL);\n    export_jobs();',1)
bench = bench.replace('/* Called under mu at a worker boundary, after the previous chunk finished. */', r'''static struct { uint64_t epoch, decision, time; int worker, cap; } milestone_effects[256];
static size_t milestone_effect_count;
static unsigned milestone_effect_dropped;

static void record_effect(int worker, int cap, uint64_t epoch, uint64_t decision, uint64_t now) {
    if (milestone_effect_count == sizeof milestone_effects / sizeof milestone_effects[0]) {
        ++milestone_effect_dropped;
        return;
    }
    size_t i = milestone_effect_count++;
    milestone_effects[i].epoch = epoch;
    milestone_effects[i].decision = decision;
    milestone_effects[i].time = now;
    milestone_effects[i].worker = worker;
    milestone_effects[i].cap = cap;
}

static void export_effects(void) {
    FILE *f = fopen("worker-effects.csv", "w");
    if (!f) { perror("worker-effects.csv"); return; }
    fputs("epoch,decision,worker,cap_pct,effective_ns\n", f);
    for (size_t i = 0; i < milestone_effect_count; ++i)
        fprintf(f, "%" PRIu64 ",%" PRIu64 ",%d,%d,%" PRIu64 "\n",
                milestone_effects[i].epoch, milestone_effects[i].decision,
                milestone_effects[i].worker, milestone_effects[i].cap, milestone_effects[i].time);
    if (fclose(f)) perror("worker-effects.csv close");
    printf("Worker effect records=%zu buffer_overflow=%u\n", milestone_effect_count, milestone_effect_dropped);
}

/* Called under mu at a worker boundary, after the previous chunk finished. */''',1)
bench = bench.replace('ack_ns[id] = now;', 'ack_ns[id] = now;\n        record_effect(id + 1, cap, cap_epoch, lease_until_ns ? cap_id : 0, now);',1)
bench = bench.replace('    export_jobs();', '    export_jobs();\n    export_effects();',1)
bench = bench.replace('static void *noncritical_worker(void *ptr) {', r'''/* Fault injection is compiled only into the on-board test harness. */
static int worker_fault;
static void *noncritical_worker(void *ptr) {''',1)
bench = bench.replace('        frame += (now - frame) / WORKER_NS * WORKER_NS;', r'''        frame += (now - frame) / WORKER_NS * WORKER_NS;
        if (id == 1 && worker_fault && level == 2 && st.level_applied == 0 &&
            ack_ns[1] < level_command_ns) {
            int fault = worker_fault;
            worker_fault = 0;
            pthread_mutex_unlock(&mu);
            if (fault == 2) return NULL;
            sleep_until(now + 40000000ULL);
            pthread_mutex_lock(&mu);
            continue;
        }''',1)
bench = bench.replace('                scenario = msg.req.arg; st.scenario = scenario;',r'''                scenario = msg.req.arg; st.scenario = scenario;
                worker_fault = scenario == PG_SC_FREEZE ? msg.req.extra : 0;''',1)
bench = bench.replace('static void *critical_worker(void *unused) {',r'''static void pin_spike_thread(const char *role, unsigned cpu) {
    if (!getenv("PG_T3_CPU_SPIKE")) return;
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) {
        perror("apply CPU spike affinity");
        exit(1);
    }
    printf("CPU placement: %s on CPU%u.\n",role,cpu);
    fflush(stdout);
}
static void *critical_worker(void *unused) {
    pin_spike_thread("critical",0);''',1)
bench = bench.replace('    int id = (int)(intptr_t)ptr;',r'''    int id = (int)(intptr_t)ptr;
    pin_spike_thread(id==0 ? "NC1" : "NC2",id==0 ? 2 : 3);
    int spike_duty = getenv("PG_T3_CPU_SPIKE") ? 93 : 90;''',1)
bench = bench.replace('scenario == PG_SC_SPIKE ? 90 : 15','scenario == PG_SC_SPIKE ? spike_duty : 15',1)
bench = bench.replace('static size_t milestone_effect_count;',
                      'static size_t milestone_effect_count;\nstatic uint64_t milestone_expiry_ns, milestone_expired_at_ns;\n'
                      'static uint64_t ftti_start_ns, ftti_first_ns, ftti_80_ns;\nstatic int32_t ftti_first_temp;',1)
bench = bench.replace('            if (scenario == PG_SC_RAMP) {',r'''            if (scenario == PG_SC_RAMP && ftti_start_ns) {
                temp = elapsed >= NS / 4 ? 80000 : 75000 + (int32_t)(elapsed / 50000);
                if (!ftti_first_ns) {
                    ftti_first_ns = n;
                    ftti_first_temp = temp;
                }
                if (temp == 80000 && !ftti_80_ns) ftti_80_ns = n;
            } else if (scenario == PG_SC_RAMP) {''',1)
bench = bench.replace('                scenario_started_ns = last_sensor_ns = now_ns(CLOCK_MONOTONIC);',r'''                scenario_started_ns = now_ns(CLOCK_MONOTONIC);
                if (scenario == PG_SC_RAMP && msg.req.extra == 1) {
                    ftti_start_ns = scenario_started_ns;
                    ftti_first_ns = ftti_80_ns = 0;
                } else {
                    ftti_start_ns = 0;
                    last_sensor_ns = scenario_started_ns;
                }''',1)
bench = bench.replace('    if (lease_until_ns && now >= lease_until_ns) {',r'''    if (lease_until_ns && now >= lease_until_ns) {
        milestone_expiry_ns = lease_until_ns;
        milestone_expired_at_ns = now;''',1)
bench = bench.replace('static void export_effects(void) {',r'''static void export_effects(void) {
    if (ftti_start_ns) {
        FILE *ramp = fopen("ftti-ramp.csv", "w");
        if (!ramp) perror("ftti-ramp.csv");
        else {
            fprintf(ramp, "%" PRIu64 ",%" PRIu64 ",%" PRId32 ",%" PRIu64 "\n",
                    ftti_start_ns, ftti_first_ns, ftti_first_temp, ftti_80_ns);
            if (fclose(ramp)) perror("ftti-ramp.csv close");
        }
    }
    if (milestone_expiry_ns) {
        FILE *lease = fopen("lease-expiry.csv", "w");
        if (!lease) perror("lease-expiry.csv");
        else {
            fprintf(lease, "%" PRIu64 ",%" PRIu64 "\n", milestone_expiry_ns, milestone_expired_at_ns);
            if (fclose(lease)) perror("lease-expiry.csv close");
        }
    }''',1)
if overhead:
    # The dedicated monitor owns acquisition in this comparison.
    begin=bench.index('static void *sampler(void *unused)')
    end=bench.index('static void sig_stop',begin)
    bench=bench[:begin]+'static void *sampler(void *unused) { (void)unused; return NULL; }\n'+bench[end:]
(out/'pgbench-instrumented.c').write_text(bench)
manager = (root/'manager/pg-manager.c').read_text()
manager = '#define _QNX_SOURCE\n'+manager
manager = manager.replace('#include <errno.h>', '#include <errno.h>\n#include <inttypes.h>',1)
manager = manager.replace('#include <unistd.h>', '#include <unistd.h>\n#include ".milestone-cpu-meter.inc"',1)
manager = manager.replace('    int result = 0;', r'''    int result = 0;
    int measure_cores = getenv("PG_T3_CPU_SPIKE") != NULL;
    if (measure_cores && !find_idle_clocks()) return 1;
    uint64_t effect_epoch = 0, effect_id = 0, effect_reply_ns = 0;''',1)
manager = manager.replace('        sample.nc_demand_pct[0] = 20;',r'''        if (measure_cores) {
            int measured = read_core_load(&sample);
            if (measured<0) { fputs("Could not read per-core idle time.\n",stderr); result=1; break; }
            if (!measured) {
                struct timespec warmup={.tv_nsec=PG_TICK};
                nanosleep(&warmup,NULL);
                continue;
            }
        }
        sample.nc_demand_pct[0] = 20;''',1)
manager = manager.replace('bench.scenario == PG_SC_SPIKE ? 90 : 15',
                          'bench.scenario == PG_SC_SPIKE ? (measure_cores ? 93 : 90) : 15',1)
manager = manager.replace('        const pg_status *want = &decision.decision;',r'''        const pg_status *want = &decision.decision;
        if (measure_cores && bench.scenario==PG_SC_SPIKE) record_spike(want,&sample);''',1)
manager = manager.replace('            if (!policy_send(policy_coid, &msg, &decision) && errno != EINVAL) { perror("PowerGuard effect"); result = 1; break; }',r'''            int record = want->state == CRITICAL && !msg.body.effect.nc1_pct && !msg.body.effect.nc2_pct &&
                (effect_epoch != msg.body.effect.epoch || effect_id != msg.body.effect.decision_id);
            if (!policy_send(policy_coid, &msg, &decision)) {
                if (errno != EINVAL) { perror("PowerGuard effect"); result = 1; break; }
            } else if (record) {
                struct timespec now;
                if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                    effect_epoch = msg.body.effect.epoch;
                    effect_id = msg.body.effect.decision_id;
                    effect_reply_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
                }
            }''',1)
manager = manager.replace('    name_close(bench_coid);\n    name_close(policy_coid);',r'''    if (measure_cores) export_spike();
    if (effect_reply_ns) {
        FILE *evidence = fopen("manager-confirmation.csv", "w");
        if (!evidence) perror("manager-confirmation.csv");
        else {
            fprintf(evidence, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", effect_epoch, effect_id, effect_reply_ns);
            if (fclose(evidence)) perror("manager-confirmation.csv close");
        }
    }
    name_close(bench_coid);
    name_close(policy_coid);''',1)
if overhead:
    manager=manager.replace('getenv("PG_T3_CPU_SPIKE") != NULL', '0').replace('msg.type = PG_SAMPLE;', 'msg.type = PG_READ;')
(out/'pg-manager-instrumented.c').write_text(manager)
qcc('-Wall','-Wextra','-Werror','-std=c11','-O2','-Itestbench',out/'pgbench-instrumented.c','-o',out/'pgbench')
qcc('-Wall','-Wextra','-Werror','-std=c11','-O2','-I.','-Iinclude','-Itestbench',out/'pg-manager-instrumented.c','-o',out/'pg-manager')
service_image=root/'build/aarch64le-debug/PowerGuard'
if overhead:
    service_source=(root/'src/PowerGuard.c').read_text().replace('#include "ipc.h"', '#include "ipc.h"\n#include "overhead-shared.h"')
    service_source=service_source.replace('    char *end;', '    overhead_shared *slot=open_overhead(0); if(!slot) return 2;\n    char *end;')
    service_source=service_source.replace('            pg_observe(', '''            if(in.pulse.code==SENSOR_PULSE) {
                pthread_mutex_lock(&slot->lock);
                pg_sample sample=slot->sample;
                pthread_mutex_unlock(&slot->lock);
                pg_ingest(&policy,&sample,monotonic_ns());
                ++slot->received;
            }
            pg_observe(''',1)
    service_image=out/'PowerGuard'
    (out/'overhead-service.c').write_text(service_source)
    qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-Iinclude','-Itests',out/'overhead-service.c',
        'supervisor/policy.c','supervisor/metrics.c','-o',service_image)
write_assets(out/'assets.h', [
    ('asset_bench',out/'pgbench'),
    ('asset_supervisor',service_image),
    ('asset_manager',out/'pg-manager'),
    ('asset_check',root/'build/aarch64le-debug/pg-check'),
])
write_assets(out/'cli-assets.h', [
    ('asset_console',root/'build/aarch64le-debug/pg-console'),
])
if overhead:
    qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-D_QNX_SOURCE','-DOVERHEAD_TRIAL',
        '-Iinclude','-Itestbench','-Itests','-I'+str(out),root/'.milestone-runner.c','supervisor/policy.c','-o',out/'pg-t3-overhead')
    sys.exit(0)
qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-DCLI_TRIAL',
    '-Iinclude','-Itestbench','-I'+str(out),root/'.milestone-runner.c','-o',out/'pg-t3-display')
if sys.argv[1:] == ['--cli']:
    sys.exit(0)
if sys.argv[1:] == ['--handoff']:
    qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-DHANDOFF_TRIAL',
        '-Iinclude','-Itestbench','-I'+str(out),root/'.milestone-runner.c','-o',out/'pg-t3-handoff')
    sys.exit(0)
qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-DCPU_SPIKE_TRIAL',
    '-Iinclude','-Itestbench','-I'+str(out),root/'.milestone-runner.c','-o',out/'pg-t3-cpu-spike')
if sys.argv[1:] == ['--spike']:
    sys.exit(0)
qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-DCRITICAL_TRIAL','-DFTTI_TRIAL',
    '-Iinclude','-Itestbench','-I'+str(out),root/'.milestone-runner.c','-o',out/'pg-t3-ftti')
if sys.argv[1:] == ['--ftti']:
    sys.exit(0)
qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-DCRITICAL_TRIAL','-DSTALE_TRIAL',
    '-Iinclude','-Itestbench','-I'+str(out),root/'.milestone-runner.c','-o',out/'pg-t3-critical')
qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-DCRITICAL_TRIAL','-DRECOVERY_TRIAL',
    '-Iinclude','-Itestbench','-I'+str(out),root/'.milestone-runner.c','-o',out/'pg-t3-recovery')
qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-DWORKER_FAULT_TRIAL',
    '-Iinclude','-Itestbench','-I'+str(out),root/'.milestone-runner.c','-o',out/'pg-t3-worker-faults')
