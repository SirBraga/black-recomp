#!/usr/bin/env python3
import os
import signal
import subprocess
import time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
runner = root / 'tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner'
disc = root / 'recomp/disc'
capture_path = root / 'recomp/diagnostics/level00-sdl-current-gif.bin'
log_path = Path('/private/tmp/black-sdl-current-capture-armed.log')
trigger_path = Path('/private/tmp/black-level00-arm.trigger')

pad = '9:up:0.3,9.5:cross:0.3,20:select:0.3,22:cross:0.3,24:cross:0.3,26:cross:0.3,30:select:0.3,40:select:0.3,46:cross:0.3,48:cross:0.3,50:cross:0.3,54:select:0.3,62:cross:0.3,64:cross:0.3,68:select:0.3,80:select:0.3'

env = os.environ.copy()
for k in list(env):
    if k.startswith(('PS2X_', 'BLACK_')):
        del env[k]

env.update({
    'BLACK_DEBUG': '1',
    'BLACK_CUTSCENE_SKIP': '1',
    'BLACK_CD_IMAGE': str(root / 'orig/Black.iso'),
    'PS2X_PAD_SCRIPT': pad,
    'PS2X_GS_SDL_GPU': '1',
    'PS2X_GIF_RAW_CAPTURE': str(capture_path),
    'PS2X_GIF_RAW_CAPTURE_TRIGGER': str(trigger_path),
})

if capture_path.exists():
    capture_path.unlink()
if trigger_path.exists():
    trigger_path.unlink()

print(f"Launching runner with trigger={trigger_path}...", flush=True)
timeout = 180
deadline = time.monotonic() + timeout

with open(log_path, 'w') as log_file:
    child = subprocess.Popen(
        [str(runner), str(disc / 'SLUS_213.76')],
        cwd=disc,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    capture_started = False
    capture_complete = False
    armed = False

    try:
        with open(log_path, 'r') as reader:
            while child.poll() is None and time.monotonic() < deadline:
                for line in reader:
                    if not armed and ('Level_00' in line or 'cur=0x004bcf78' in line):
                        armed = True
                        trigger_path.touch()
                        print(f"[{time.strftime('%H:%M:%S')}] Armed trigger at: {line.strip()}", flush=True)
                    elif '[gif-raw] recording level-texture' in line:
                        capture_started = True
                        print(f"[{time.strftime('%H:%M:%S')}] {line.strip()}", flush=True)
                    elif '[gif-raw] bounded capture complete' in line:
                        capture_complete = True
                        print(f"[{time.strftime('%H:%M:%S')}] {line.strip()}", flush=True)
                        break
                    elif '[black-init]' in line and 'state=' in line and not armed:
                        # Periodic update
                        pass
                if capture_complete:
                    print("Bounded capture completed! Shutting down runner...", flush=True)
                    time.sleep(1)
                    break
                time.sleep(1)
    finally:
        if child.poll() is None:
            print("Terminating runner...", flush=True)
            try:
                os.killpg(child.pid, signal.SIGTERM)
                child.wait(timeout=5)
            except (subprocess.TimeoutExpired, ProcessLookupError):
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=2)
                except Exception:
                    pass

print("Finished run.")
if capture_path.exists():
    print(f"Capture created: {capture_path.stat().st_size} bytes")
else:
    print("Warning: Capture file was not created!")
