# POWERGUARD  /  TEAM SCHEDULIX
# --------------------------------------------------------------------------
# .milestone-build.py
# Original software (c) 2026 Abdul Aleem. MIT License.
# Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.

from pathlib import Path
import subprocess
import sys
import os

root = Path.cwd()
overhead = sys.argv[1:] == ['--overhead']
out = root / 'build' / ('overhead' if overhead else 'milestone')
out.mkdir(parents=True, exist_ok=True)
profile = os.environ.get("PG_BUILD_PROFILE", "debug")
if profile not in {"debug", "release"}:
    raise SystemExit("PG_BUILD_PROFILE must be debug or release")
subprocess.run(["make", "BUILD_PROFILE="+profile, "all"], check=True)
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


trial_flags = ['-Wall', '-Wextra', '-Werror', '-std=gnu11', '-O2', '-Iinclude', '-Itestbench',
               '-DPG_BENCH_TRIAL']
if overhead:
    trial_flags.append('-DOVERHEAD_TRIAL')

qcc(*trial_flags, 'testbench/pgbench.c', 'platform/progress.c', '-o', out/'pgbench')
qcc(*trial_flags, '-I.', 'manager/pg-manager.c', 'platform/threadctl.c', '-o', out/'pg-manager')

service_image = root / ('build/aarch64le-' + profile) / 'PowerGuard'
if overhead:
    service_image = out/'PowerGuard'
    qcc(*trial_flags, '-Itests', 'src/PowerGuard.c', 'supervisor/policy.c', 'supervisor/metrics.c',
        'platform/threadctl.c', 'platform/progress.c', '-o', service_image)

write_assets(out/'assets.h', [
    ('asset_bench',out/'pgbench'),
    ('asset_supervisor',service_image),
    ('asset_manager',out/'pg-manager'),
    ('asset_check',root/('build/aarch64le-'+profile)/'pg-check'),
] + ([('asset_supervisor_check',root/('build/aarch64le-'+profile)/'pg-supervisor-check')] if overhead else []))
write_assets(out/'cli-assets.h', [
    ('asset_console',root/('build/aarch64le-'+profile)/'pg-console'),
])
if sys.argv[1:] == ['--ham-suite']:
    flags = ['-Wall','-Wextra','-Werror','-std=gnu11','-O2','-Iinclude','-Itestbench','-I'+str(out)]
    trials = [('throttled',[]),('critical',['-DCRITICAL_TRIAL']),
              ('recovery',['-DCRITICAL_TRIAL','-DRECOVERY_TRIAL'])]
    for label,defines in trials:
        qcc(*flags,'-DHAM_ON_TRIAL',*defines,root/'.milestone-runner.c','-o',out/('ham-'+label))
    write_assets(out/'ham-suite-assets.h',[(label,out/('ham-'+label)) for label,_ in trials])
    qcc(*flags,root/'.milestone-ham-suite.c','-o',out/'pg-t3-ham-suite')
    sys.exit(0)

if overhead:
    qcc('-Wall','-Wextra','-Werror','-std=gnu11','-O2','-D_QNX_SOURCE','-DOVERHEAD_TRIAL',
        '-Iinclude','-Itestbench','-Itests','-I'+str(out),root/'.milestone-runner.c','supervisor/policy.c','platform/progress.c','-o',out/'pg-t3-overhead')
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
