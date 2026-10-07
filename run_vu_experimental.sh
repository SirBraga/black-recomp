#!/usr/bin/env bash
# Limited interactive run with opt-in VU AOT and the existing GPU backend.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ -f "$ROOT/tools/PS2Recomp/ps2xRuntime/src/lib/vu/black_vu_aot_generated.hpp" ]] || { echo 'Gere e compile primeiro com build_vu_experimental.sh.' >&2; exit 1; }
export PS2X_VU_AOT=1
export PS2X_GS_PARALLEL=1
export PS2X_GS_PARALLEL_MODULE="${PS2X_GS_PARALLEL_MODULE:-$ROOT/recomp/gpu/build/libblack-parallel-gs.so}"
export PS2X_GS_MOLTENVK="${PS2X_GS_MOLTENVK:-$ROOT/recomp/gpu/moltenvk/libMoltenVK.dylib}"
exec "$ROOT/ps2recomp/run.sh" "${1:-120}" "${2:-/tmp/black_vu_experimental.log}"
