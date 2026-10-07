#!/usr/bin/env python3
"""Compare every VRAM checkpoint using the real serial and parallel GS backends."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory); exe=d/'gs-test'
 subprocess.run(['clang++','-std=c++20','-O3','-pthread','-w','-I',str(root/'tools/PS2Recomp/ps2xRuntime/include'),str(root/'ps2recomp/tests/gs_triangle_test.cpp'),str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp'),str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp'),'-o',str(exe)],check=True)
 for mode in range(4):
  baseline=None
  for workers in (0,1,4,4):
   out=d/f'vram-{workers}.bin'
   run=subprocess.run([str(exe),str(out)],env=dict(os.environ,PS2X_GS_RASTER_THREADS=str(workers),GS_TEST_ROUND=str(mode)),capture_output=True,text=True,check=True)
   current=out.read_bytes()
   if baseline is None: baseline=current
   assert current==baseline, f'VRAM mismatch with {workers} workers, rounding mode {mode}'
   print(f'rounding={mode} workers={workers}: {run.stderr.strip()}')
print('PASS: serial/parallel VRAM checkpoints identical, including feedback, aliases and all rounding modes')
