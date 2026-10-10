#!/usr/bin/env python3
"""Clean performance run: the game with the launcher's settings (no debug hooks, no captures), driven into
Level_00 by the pad script, then sampled.

  perf_run.py <tag> [VAR=value ...]     e.g. perf_run.py tick60 BLACK_TICK_RATE=60

Prints the game's updates per second (BLACK_FPS), how busy each thread is and what the busy ones wait on.
WARMUP=<s> (default 100) is how long to wait before sampling, SAMPLE=<s> (default 8) how long to sample.
The sample is kept in recomp/diagnostics/perf/<tag>.sample.txt (sample_threads.py / sample_waits.py read it).
"""
import os, shutil, signal, subprocess, sys, time
from pathlib import Path
root = Path(__file__).resolve().parents[2]
out = root / 'recomp/diagnostics/perf'; out.mkdir(parents=True, exist_ok=True)
tag = sys.argv[1] if len(sys.argv) > 1 else 'run'
runner = root / 'tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner'
alias = runner.with_name('blackPerf'); shutil.copy2(runner, alias)
disc = root / 'recomp/disc'
pad = '9:up:0.3,9.5:cross:0.3,20:select:0.3,22:cross:0.3,24:cross:0.3,26:cross:0.3,30:select:0.3,40:select:0.3,46:cross:0.3,48:cross:0.3,50:cross:0.3,54:select:0.3,62:cross:0.3,64:cross:0.3,68:select:0.3,80:select:0.3,88:r1:0.3,94:r1:0.3,100:r1:0.3'
env = {k: v for k, v in os.environ.items() if not k.startswith(('PS2X_', 'BLACK_'))}
env.update({'BLACK_CD_IMAGE': str(root / 'orig/Black.iso'), 'BLACK_CUTSCENE_SKIP': '1', 'BLACK_FPS': '1',
            'PS2X_PAD_SCRIPT': pad, 'PS2X_VIF1_THREAD': '1', 'PS2X_IOP_QUANTUM': '512',
            'PS2X_GS_PARALLEL': '1', 'PS2X_GS_PARALLEL_MODULE': str(root / 'recomp/gpu/build/libblack-parallel-gs.so'),
            'PS2X_GS_MOLTENVK': str(root / 'recomp/gpu/moltenvk/libMoltenVK.dylib')})
if os.environ.get('POSE'):
    env['BLACK_POSE_REF'] = os.environ['POSE']
    env['BLACK_WIDESCREEN'] = '1'
    Path(os.environ['POSE'] + '.trigger').unlink(missing_ok=True)
for assignment in sys.argv[2:]:
    key, _, value = assignment.partition('=')
    env[key] = value
log = out / f'{tag}.log'
child = subprocess.Popen([str(alias), str(disc / 'SLUS_213.76')], cwd=disc, env=env, stdout=open(log, 'w'),
                         stderr=subprocess.STDOUT, start_new_session=True)
# POSE=<RAM image>: once the level has loaded, move the player to the position saved in that image
# (BLACK_POSE_REF), e.g. recomp/diagnostics/perf/heavy-spot.ram, the heavy view the user found.
pose = os.environ.get('POSE')
try:
    if pose:
        time.sleep(78)
        Path(pose + '.trigger').touch()
        time.sleep(max(0, int(os.environ.get('WARMUP', '100')) - 78))
    else:
        time.sleep(int(os.environ.get('WARMUP', '100')))
    sample = out / f'{tag}.sample.txt'
    subprocess.run(['sample', str(child.pid), os.environ.get('SAMPLE', '8'), '-file', str(sample)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
finally:
    try: os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    try: child.wait(timeout=10)
    except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL)
fps = [l.strip() for l in open(log, errors='replace') if 'black-fps' in l]
print('updates/s (last 8 readings):', ' '.join(l.split('=')[-1] for l in fps[-8:]))
here = Path(__file__).resolve().parent
subprocess.run([sys.executable, str(here / 'sample_threads.py'), str(sample), '6'])
subprocess.run([sys.executable, str(here / 'sample_waits.py'), str(sample)])
