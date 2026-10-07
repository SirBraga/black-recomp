#!/usr/bin/env python3
"""CPU GS: decoded texture cache matches a fresh sample after VRAM writes."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / "gs-texcache-test"
    subprocess.run(
        [
            "clang++", "-std=c++20", "-O2", "-pthread", "-w",
            "-I", str(root / "tools/PS2Recomp/ps2xRuntime/include"),
            str(root / "ps2recomp/tests/gs_texcache_test.cpp"),
            str(root / "tools/PS2Recomp/ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp"),
            str(root / "tools/PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp"),
            "-o", str(exe),
        ],
        check=True,
    )
    subprocess.run(
        [str(exe)],
        env=dict(os.environ, PS2X_GS_RASTER_THREADS="0", PS2X_GS_METAL="0"),
        check=True,
    )
    subprocess.run(
        [str(exe)],
        env=dict(os.environ, PS2X_GS_RASTER_THREADS="4", PS2X_GS_METAL="0"),
        check=True,
    )
