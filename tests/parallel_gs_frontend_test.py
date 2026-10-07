#!/usr/bin/env python3
"""Exercise the actual GS frontend and optional GPU module on Apple silicon."""
from pathlib import Path
import os, platform, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
module=root/'recomp/gpu/build/libblack-parallel-gs.so'
molten=Path(os.environ.get('PS2X_GS_MOLTENVK',root/'recomp/gpu/moltenvk/libMoltenVK.dylib'))
if platform.system()!='Darwin' or platform.machine()!='arm64' or not module.exists() or not molten.exists():
 print('SKIP: requires Apple silicon and built paraLLEl-GS/MoltenVK module');raise SystemExit(0)
rt=root/'tools/PS2Recomp/ps2xRuntime'
with tempfile.TemporaryDirectory() as directory:
 exe=Path(directory)/'frontend-test'
 sources=[root/'ps2recomp/tests/parallel_gs_frontend_test.cpp']+[rt/'src/lib/gs'/f for f in ['gs_frontend.cpp','gs_cpu_backend.cpp','gs_threaded_backend.cpp','ps2_gs_memory.cpp']]
 subprocess.run(['clang++','-std=c++20','-O1','-w','-pthread','-DUSE_SSE2NEON','-I'+str(root/'tools/PS2Recomp/out/rt/_deps/sse2neon-src'),'-I'+str(rt/'include'),*map(str,sources),'-o',str(exe)],check=True)
 subprocess.run([str(exe),str(module),str(molten)],check=True)
