#!/usr/bin/env python3
"""A/B test for PS2X_PATH3_EOP_GATE (default on since 2026-10-07): build, run baseline and gated Level_00, measure CLUT integrity."""
import os, signal, subprocess, time, sys, re
from pathlib import Path
root = Path(__file__).resolve().parents[2]
diag = root / 'recomp/diagnostics'
out = diag / 'path3-gate'
out.mkdir(exist_ok=True)
status = out / 'STATUS.txt'
def say(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(status, 'a') as f: f.write(line + '\n')
if not os.environ.get('STATUS_APPEND'): status.write_text('')
say('building ps2EntryRunner')
b = subprocess.run([str(root/'.venv/bin/ninja'), '-C', str(root/'tools/PS2Recomp/out/rt'), '-j8', 'ps2EntryRunner'],
                   stdout=open(out/'build.log','w'), stderr=subprocess.STDOUT)
if b.returncode != 0:
    say(f'BUILD FAILED rc={b.returncode} (see build.log)'); sys.exit(1)
say('build ok')
runner = root / 'tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner'
# Run a copy under another process name: other recomp checkouts on the same machine build a binary
# with the same name, and name-wide kills from their scripts would take this run down too.
import shutil
alias = runner.with_name('blackRunner')
shutil.copy2(runner, alias)
runner = alias
disc = root / 'recomp/disc'
pad = '9:up:0.3,9.5:cross:0.3,20:select:0.3,22:cross:0.3,24:cross:0.3,26:cross:0.3,30:select:0.3,40:select:0.3,46:cross:0.3,48:cross:0.3,50:cross:0.3,54:select:0.3,62:cross:0.3,64:cross:0.3,68:select:0.3,80:select:0.3'
pad = os.environ.get('PAD', pad)  # PAD=<script> replaces the default input script
modes = sys.argv[1:] or ['baseline', 'gate']
hold = int(os.environ.get('HOLD', '75'))
arm_after = int(os.environ.get('ARM_AFTER', '0'))
for mode in modes:
    cap = out / f'{mode}-gif.bin'; trig = out / f'{mode}.trigger'; log = out / f'{mode}.log'
    for p in (cap, trig):
        if p.exists(): p.unlink()
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS2X_', 'BLACK_'))}
    env.update({'BLACK_DEBUG': '1', 'BLACK_CUTSCENE_SKIP': '1', 'BLACK_CD_IMAGE': str(root/'orig/Black.iso'),
                'PS2X_PAD_SCRIPT': pad, 'PS2X_GS_SDL_GPU': '1', 'PS2X_PATH3_GATE_STATS': '1',
                'PS2X_GIF_RAW_CAPTURE': str(cap), 'PS2X_GIF_RAW_CAPTURE_TRIGGER': str(trig)})
    # modes: baseline = gate off, gate = gate forced on, default = variable unset (gate is on by default)
    if mode != 'default': env['PS2X_PATH3_EOP_GATE'] = '1' if mode == 'gate' else '0'
    # EXTRA_<VAR>=x passes VAR=x to the runner (e.g. EXTRA_BLACK_DUMP_EVERY=5 for finer upd= sampling).
    env.update({k[6:]: v for k, v in os.environ.items() if k.startswith('EXTRA_')})
    for k in os.environ.get('UNSET', '').split(','):  # UNSET=VAR1,VAR2 removes defaults (e.g. PS2X_GS_SDL_GPU)
        env.pop(k, None)
    for f in Path('/tmp').glob('black_frame_*.ppm'): f.unlink()
    say(f'RUN {mode} starting')
    child = subprocess.Popen([str(runner), str(disc/'SLUS_213.76')], cwd=disc, env=env,
                             stdout=open(log,'w'), stderr=subprocess.STDOUT, start_new_session=True)
    t0 = time.monotonic(); armed_at = None
    try:
        with open(log, 'r', errors='replace') as r:
            while child.poll() is None:
                for line in r:
                    if armed_at is None and ('Level_00' in line or 'cur=0x004bcf78' in line):
                        armed_at = time.monotonic()
                        if arm_after <= 0: trig.touch()
                        say(f'RUN {mode} reached Level_00 at {armed_at-t0:.0f}s; screenshot window open for {hold}s')
                # ARM_AFTER=<s>: start the GIF capture that many seconds after reaching the level.
                if armed_at and arm_after > 0 and not trig.exists() and time.monotonic() - armed_at > arm_after:
                    trig.touch(); say(f'RUN {mode} capture armed {arm_after}s into the level')
                    if os.environ.get('RAM_TRIGGER'): Path(os.environ['RAM_TRIGGER']).touch()  # arms PS2X_RAM_DUMP
                if armed_at and time.monotonic() - armed_at > hold: break
                if time.monotonic() - t0 > int(os.environ.get('TIMEOUT', '240')): say(f'RUN {mode} timeout'); break
                time.sleep(1)
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL)
    say(f'RUN {mode} finished')
    for f in sorted(Path('/tmp').glob('black_frame_*.ppm'), key=lambda f: f.stat().st_mtime):
        (out / f"{mode}-{f.stem.replace('black_frame_', 'frame')}.ppm").write_bytes(f.read_bytes())
    if cap.exists():
        seq = out / f'{mode}-transfers.bin'
        subprocess.run([sys.executable, '-I', str(root/'ps2recomp/diagnostics/audit_texture_sequence.py'), str(cap),
                        '--all-transfers', '--extract', str(seq)], stdout=subprocess.DEVNULL)
        res = subprocess.run([sys.executable, '-I', str(root/'ps2recomp/diagnostics/clut_last_writer.py'), str(seq)+'.seq'],
                             capture_output=True, text=True).stdout
        say(f'RESULT {mode} CLUT: ' + res.replace('\n', ' | ')[:600])
        if not os.environ.get('KEEP_CAPTURE'):  # KEEP_CAPTURE=1 keeps <mode>-gif.bin and the extracted transfers
            cap.unlink()
            Path(str(seq)).unlink(missing_ok=True)
    gate_lines = [l for l in open(log, errors='replace') if '[path3-gate]' in l]
    if gate_lines: say(f'RESULT {mode} gate-stats: ' + gate_lines[-1].strip())
    upd = [l.strip() for l in open(log, errors='replace') if 'upd=' in l][-6:]
    for u in upd: say(f'  {mode} {u[:200]}')
say('ALL DONE')
