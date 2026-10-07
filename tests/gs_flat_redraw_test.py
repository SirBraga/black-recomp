#!/usr/bin/env python3
"""A second submit of the same triangle with a flat UV must not replace the first."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
rt = root / "tools/PS2Recomp/ps2xRuntime"
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / "flat-redraw"
    sources = [root / "ps2recomp/tests/gs_flat_redraw_test.cpp"] + [
        rt / "src/lib/gs" / name
        for name in ("gs_frontend.cpp", "gs_cpu_backend.cpp", "gs_threaded_backend.cpp", "ps2_gs_memory.cpp")
    ]
    subprocess.run(
        [
            "clang++", "-std=c++20", "-O2", "-pthread", "-w", "-DUSE_SSE2NEON",
            "-I", str(root / "tools/PS2Recomp/out/rt/_deps/sse2neon-src"),
            "-I", str(rt / "include"), *map(str, sources), "-o", str(exe),
        ],
        check=True,
    )
    env = dict(os.environ)
    env.pop("PS2X_GS_PARALLEL", None)
    env["PS2X_GS_METAL"] = "0"
    env["PS2X_GS_THREAD"] = "0"
    subprocess.run([str(exe)], check=True, env=env)
