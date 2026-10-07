#!/usr/bin/env python3
"""Real GS backend: reversed vertex ordering must preserve a known texture gradient."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
 exe=Path(directory)/'gs-sprite-test'
 subprocess.run(['clang++','-std=c++20','-O2','-pthread','-w','-I',str(root/'tools/PS2Recomp/ps2xRuntime/include'),str(root/'ps2recomp/tests/gs_sprite_test.cpp'),str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp'),str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp'),'-o',str(exe)],check=True)
 for gpu in ('0','1'):
  subprocess.run([str(exe)],env=dict(os.environ,PS2X_GS_RASTER_THREADS='0',PS2X_GS_METAL=gpu,PS2X_GS_METAL_MIN_PIXELS='0'),check=True)
