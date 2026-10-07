#!/usr/bin/env python3
"""Compare actual Metal compute dispatches with the CPU GS, on Apple silicon."""
from pathlib import Path
import subprocess,tempfile,platform
root=Path(__file__).resolve().parents[2]
if platform.system()!='Darwin' or platform.machine()!='arm64':
 print('SKIP: real Metal validation requires Apple silicon');raise SystemExit(0)
with tempfile.TemporaryDirectory() as directory:
 exe=Path(directory)/'gs-metal-test'
 subprocess.run(['clang++','-std=c++20','-O2','-pthread','-w','-I',str(root/'tools/PS2Recomp/ps2xRuntime/include'),str(root/'ps2recomp/tests/gs_metal_test.cpp'),str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp'),str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
