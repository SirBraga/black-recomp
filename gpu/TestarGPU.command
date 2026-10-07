#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export PS2X_GS_PARALLEL=1
export PS2X_GS_PARALLEL_MODULE="$ROOT/recomp/gpu/build/libblack-parallel-gs.so"
export PS2X_GS_MOLTENVK="${PS2X_GS_MOLTENVK:-$ROOT/recomp/gpu/moltenvk/libMoltenVK.dylib}"
export BLACK_CUTSCENE_SKIP=1
export PS2X_CALLBACK_QUEUE_TRACE=1
# Full packet recording is opt-in: continuous movie uploads fill the bounded capture quickly.
# Set PS2X_PARALLEL_STREAM_CAPTURE to an absolute file path when recording a reproduction.
export PS2X_PARALLEL_BAD_TRANSFER_CAPTURE="$ROOT/recomp/diagnostics/parallel-memory-card-invalid.bin"
mkdir -p "$ROOT/recomp/diagnostics"
unset PS2X_GS_METAL PS2X_GS_THREAD
[[ -f "$PS2X_GS_PARALLEL_MODULE" && -f "$PS2X_GS_MOLTENVK" ]] || { echo 'Build GPU module and supply MoltenVK first.' >&2; exit 1; }
exec "$ROOT/ps2recomp/run.sh" 3600 "$ROOT/recomp/parallel-gpu-test.log"
