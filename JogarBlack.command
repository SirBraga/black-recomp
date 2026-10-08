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
if [[ "${BLACK_GS:-auto}" != "sdlgpu" && -f "$GS_MODULE" && -f "$GS_MOLTENVK" ]]; then
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
cd "$DISC"
exec "$RUNNER" "$DISC/SLUS_213.76"
