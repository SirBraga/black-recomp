#!/usr/bin/env python3
"""CPU GS: TEX1 LOD selects MIPTBP1 level 1, and a negative K stays on level 0."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / "gs-mip-test"
    subprocess.run(
        [
            "clang++", "-std=c++20", "-O2", "-pthread", "-w",
            "-I", str(root / "tools/PS2Recomp/ps2xRuntime/include"),
            str(root / "ps2recomp/tests/gs_mip_test.cpp"),
            str(root / "tools/PS2Recomp/ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp"),
            str(root / "tools/PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp"),
            "-o", str(exe),
        ],
        check=True,
    )
    subprocess.run(
        [str(exe)],
        env=dict(os.environ, PS2X_GS_RASTER_THREADS="0", PS2X_GS_METAL="0", PS2X_GS_DENSITY_LOD="1"),
        check=True,
    )
