#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$ROOT/tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner"
DISC="$ROOT/recomp/disc"
export BLACK_CD_IMAGE="$ROOT/orig/Black.iso"
# GS renderer: paraLLEl-GS (Vulkan compute over MoltenVK) when its module is built, about twice as fast as
# the in-tree SDL_GPU kernels in Level_00; BLACK_GS=sdlgpu forces the SDL_GPU/Metal renderer.
GS_MODULE="$ROOT/recomp/gpu/build/libblack-parallel-gs.so"
GS_MOLTENVK="${PS2X_GS_MOLTENVK:-$ROOT/recomp/gpu/moltenvk/libMoltenVK.dylib}"
unset PS2X_PAD_SCRIPT PS2X_GS_THREAD PS2X_GS_METAL PS2X_GS_PARALLEL PS2X_GS_PARALLEL_MODULE PS2X_GS_MOLTENVK PS2X_GS_SDL_GPU
# VIF1/VU1/GS work on its own thread (the game thread only builds the display lists); BLACK_VIF1_THREAD=0 disables.
if [[ "${BLACK_VIF1_THREAD:-1}" != "0" ]]; then export PS2X_VIF1_THREAD=1; else unset PS2X_VIF1_THREAD; fi
# BLACK_GS=native (experimental): the native Metal renderer (hardware rasterizer), drawn straight to the window at
# BLACK_GS_SCALE times the game's resolution (default 2). See ps2recomp/GS_NATIVE_RENDERER_PLAN.md.
NATIVE_MODULE="$ROOT/recomp/gpu/build/libblack-native-gs.so"
unset PS2X_GS_NATIVE_PRESENT PS2X_GS_NATIVE_SCALE
if [[ "${BLACK_GS:-auto}" == "native" && -f "$NATIVE_MODULE" ]]; then
    export PS2X_GS_PARALLEL=1 PS2X_GS_PARALLEL_MODULE="$NATIVE_MODULE" PS2X_GS_MOLTENVK="$GS_MOLTENVK" PS2X_GS_NATIVE_PRESENT=1 PS2X_GS_NATIVE_SCALE="${BLACK_GS_SCALE:-2}"
elif [[ "${BLACK_GS:-auto}" != "sdlgpu" && -f "$GS_MODULE" && -f "$GS_MOLTENVK" ]]; then
    export PS2X_GS_PARALLEL=1 PS2X_GS_PARALLEL_MODULE="$GS_MODULE" PS2X_GS_MOLTENVK="$GS_MOLTENVK"
else
    export PS2X_GS_SDL_GPU=1
fi
if [[ "${BLACK_DEBUG:-}" == "1" ]]; then
    export BLACK_DEBUG=1
else
    unset BLACK_DEBUG
fi
# VU microprograms the runner has no recompiled code for are interpreted (slow) and saved here, so that
# ps2recomp/build_vu_recompiled.sh can add them to the next build.
mkdir -p "$ROOT/recomp/diagnostics/vu-captured"
export PS2X_VU_RECOMP_CAPTURE="${PS2X_VU_RECOMP_CAPTURE:-$ROOT/recomp/diagnostics/vu-captured}"
# Same for IOP modules (their relocated text; ps2recomp/build_iop_recompiled.sh uses it).
mkdir -p "$ROOT/recomp/diagnostics/iop-captured"
export PS2X_IOP_RECOMP_CAPTURE="${PS2X_IOP_RECOMP_CAPTURE:-$ROOT/recomp/diagnostics/iop-captured}"
# IOP scheduling quantum in IOP cycles (runtime default 128): 512 is ~14 us of guest time and cuts the
# scheduler overhead of an IOP that is idle most of the time.
export PS2X_IOP_QUANTUM="${PS2X_IOP_QUANTUM:-512}"
export BLACK_CUTSCENE_SKIP="${BLACK_CUTSCENE_SKIP:-1}"
# If the game jumps to a bad address (it freezes there), keep the RAM and the last calls for diagnosis.
export PS2X_FAULT_RAM_DUMP="${PS2X_FAULT_RAM_DUMP:-$ROOT/recomp/diagnostics/fault.ram}"
export PS2X_DISPATCH_HISTORY="${PS2X_DISPATCH_HISTORY:-1}"
# BLACK_CAPTURE=1: record the GS command stream (about 4 s, 256 MiB cap) from the moment the file
# recomp/diagnostics/user-capture.trigger is created, plus a RAM dump, to diagnose a visual defect:
#   touch recomp/diagnostics/user-capture.trigger   (while the defect is on screen)
if [[ "${BLACK_CAPTURE:-}" == "1" ]]; then
    rm -f "$ROOT/recomp/diagnostics/user-capture.bin" "$ROOT/recomp/diagnostics/user-capture.trigger" \
          "$ROOT/recomp/diagnostics/user-capture.ram" "$ROOT/recomp/diagnostics/user-capture.ram.trigger"
    export PS2X_GIF_RAW_CAPTURE="$ROOT/recomp/diagnostics/user-capture.bin"
    export PS2X_GIF_RAW_CAPTURE_TRIGGER="$ROOT/recomp/diagnostics/user-capture.trigger"
    export PS2X_RAM_DUMP="$ROOT/recomp/diagnostics/user-capture.ram"
fi
cd "$DISC"
# BLACK_WATCH=1: run under lldb with a watchpoint on the render-queue bucket table (diagnostics/watch_bucket.py), to
# find what overwrites it before the game freezes. The report goes to the log; the RAM to recomp/diagnostics/watch-fault.ram.
if [[ "${BLACK_WATCH:-}" == "1" ]]; then
    export BLACK_WATCH_RAM="$ROOT/recomp/diagnostics/watch-fault.ram"
    exec lldb -b -o "command script import $ROOT/ps2recomp/diagnostics/watch_bucket.py" -o run -- "$RUNNER" "$DISC/SLUS_213.76"
fi
exec "$RUNNER" "$DISC/SLUS_213.76"
