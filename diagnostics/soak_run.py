#!/usr/bin/env python3
"""Soak test: the game with the launcher's settings, driven into Level_00 and then fed pseudo-random pad
input (moving, turning, shooting, reloading, grenades) until it stops or the time is up.

  soak_run.py <tag> [VAR=value ...]     SECONDS=<n> (default 360)  SEED=<n> (default 1)

Reports whether the log shows a missing branch target, a reserved VU instruction or a dead game thread
(updates/s no longer printed), and when. The log is recomp/diagnostics/perf/<tag>.log.
"""
import os, random, re, shutil, signal, subprocess, sys, time
from pathlib import Path
root = Path(__file__).resolve().parents[2]
out = root / 'recomp/diagnostics/perf'; out.mkdir(parents=True, exist_ok=True)
tag = sys.argv[1] if len(sys.argv) > 1 else 'soak'
seconds = int(os.environ.get('SECONDS_', os.environ.get('SOAK_SECONDS', '360')))
src = (root / 'ps2recomp/diagnostics/perf_run.py').read_text()
pad = re.search(r"^pad = '([^']*)'", src, re.M).group(1)
rng = random.Random(int(os.environ.get('SEED', '1')))
t = 72.0
actions = []
while t < 72.0 + seconds:
    kind = rng.random()
    if kind < 0.35:
        actions.append('%.1f:%s:%.1f' % (t, rng.choice(['ls_up', 'ls_up', 'ls_left', 'ls_right', 'ls_down']), rng.uniform(0.4, 2.0)))
    elif kind < 0.60:
        actions.append('%.1f:%s:%.1f' % (t, rng.choice(['rs_left', 'rs_right', 'rs_up', 'rs_down']), rng.uniform(0.2, 0.9)))
    elif kind < 0.85:
        actions.append('%.1f:r1:%.1f' % (t, rng.uniform(0.2, 1.5)))
    else:
        actions.append('%.1f:%s:0.3' % (t, rng.choice(['square', 'l1', 'r2', 'l2', 'triangle', 'circle', 'cross'])))
    t += rng.uniform(0.3, 1.2)
runner = root / 'tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner'
alias = runner.with_name('blackSoak_' + tag); shutil.copy2(runner, alias)
disc = root / 'recomp/disc'
env = {k: v for k, v in os.environ.items() if not k.startswith(('PS2X_', 'BLACK_'))}
env.update({'BLACK_CD_IMAGE': str(root / 'orig/Black.iso'), 'BLACK_CUTSCENE_SKIP': '1', 'BLACK_FPS': '1',
            'PS2X_PAD_SCRIPT': pad + ',' + ','.join(actions), 'PS2X_VIF1_THREAD': '1', 'PS2X_IOP_QUANTUM': '512',
            'PS2X_GS_PARALLEL': '1', 'PS2X_GS_PARALLEL_MODULE': str(root / 'recomp/gpu/build/libblack-parallel-gs.so'),
            'PS2X_GS_MOLTENVK': str(root / 'recomp/gpu/moltenvk/libMoltenVK.dylib')})
if os.environ.get('POSE'):
    env['BLACK_POSE_REF'] = os.environ['POSE']
    Path(os.environ['POSE'] + '.trigger').unlink(missing_ok=True)
for assignment in sys.argv[2:]:
    key, _, value = assignment.partition('=')
    env[key] = value
log = out / f'{tag}.log'
child = subprocess.Popen([str(alias), str(disc / 'SLUS_213.76')], cwd=disc, env=env, stdout=open(log, 'w'),
                         stderr=subprocess.STDOUT, start_new_session=True)
start = time.monotonic(); verdict = 'ran the whole time'
# POSE=<RAM image>: after the level has loaded, move the player to the position saved in that image
# (BLACK_POSE_REF of the cull probe override) so the run happens in a chosen spot.
pose = os.environ.get('POSE')
try:
    lastFps = 0; lastChange = start
    while time.monotonic() - start < 80 + seconds:
        time.sleep(5)
        if pose and time.monotonic() - start > 78:
            Path(pose + '.trigger').touch(); pose = None
        text = open(log, errors='replace').read()
        if 'missing-target' in text: verdict = 'missing branch target'; break
        if child.poll() is not None: verdict = 'process exited (%s)' % child.returncode; break
        fps = text.count('black-fps')
        if fps != lastFps: lastFps, lastChange = fps, time.monotonic()
        elif time.monotonic() - lastChange > 20: verdict = 'game thread stopped updating'; break
finally:
    elapsed = time.monotonic() - start
    try: os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    try: child.wait(timeout=10)
    except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL)
    alias.unlink(missing_ok=True)
text = open(log, errors='replace').read()
print('%s: %s after %.0f s; reserved VU instructions: %d' % (tag, verdict, elapsed, text.count('reserved lower') + text.count('reserved upper')))
for line in text.splitlines():
    if 'missing-target' in line or 'reserved lower' in line: print('   ', line[:200])
