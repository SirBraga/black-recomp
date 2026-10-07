#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$ROOT/tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner"
DISC="$ROOT/recomp/disc"
export BLACK_CD_IMAGE="$ROOT/orig/Black.iso"
unset PS2X_PAD_SCRIPT PS2X_GS_THREAD PS2X_GS_METAL PS2X_GS_PARALLEL PS2X_GS_PARALLEL_MODULE PS2X_GS_MOLTENVK
export PS2X_GS_SDL_GPU=1
if [[ "${BLACK_DEBUG:-}" == "1" ]]; then
    export BLACK_DEBUG=1
else
    unset BLACK_DEBUG
fi
export BLACK_CUTSCENE_SKIP="${BLACK_CUTSCENE_SKIP:-1}"
cd "$DISC"
exec "$RUNNER" "$DISC/SLUS_213.76"
