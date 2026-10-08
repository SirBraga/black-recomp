#!/usr/bin/env bash
# Regenerates the statically recompiled VU0/VU1 microcode
# (tools/PS2Recomp/ps2xRuntime/src/lib/vu/black_vu1_recompiled.inc) from every
# captured image, validates it against the reference interpreter on the
# recorded trace, and rebuilds the runner.
#
# Image sources (all local, never committed: they contain game code):
#   recomp/diagnostics/vu-coverage-level00/  PS2X_VU_CATALOG capture (images + coverage)
#   recomp/diagnostics/vu-trace/images/      exported from a PS2X_VU_TRACE recording
#   recomp/diagnostics/vu-captured/          PS2X_VU_RECOMP_CAPTURE=<this dir> (unknown images seen in game)
#   recomp/diagnostics/vu-programs/          microprograms extracted from the executable (extract_vu_programs.py)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tools/PS2Recomp/out/rt/ps2xRuntime"
OUT="$ROOT/tools/PS2Recomp/ps2xRuntime/src/lib/vu/black_vu1_recompiled.inc"
NINJA="$ROOT/.venv/bin/ninja"
D="$ROOT/recomp/diagnostics"
TRACES=("$D"/vu-trace/*.bin)   # PS2X_VU_TRACE / PS2X_VU0_TRACE recordings
mkdir -p "$D/vu-trace/images" "$D/vu-captured"
shopt -s nullglob
TRACES=("$D"/vu-trace/*.bin)

"$NINJA" -C "$ROOT/tools/PS2Recomp/out/rt" -j8 ps2x_vu1_recompile ps2x_vu1_trace_replay
for TRACE in "${TRACES[@]}"; do
  mkdir -p "$D/vu-trace/images/$(basename "$TRACE" .bin)"
  "$BIN/ps2x_vu1_trace_replay" "$TRACE" --export "$D/vu-trace/images/$(basename "$TRACE" .bin)" >/dev/null
done

ARGS=()
for coverage in "$D/vu-coverage-level00/coverage.bin" "$D"/vu-trace/images/*/coverage.bin; do
  [[ -f "$coverage" ]] && ARGS+=(--coverage "$coverage")
done
# VU1: every microprogram in the executable, extracted from its VIF MPG commands (no need to play to
# find them); they are matched by content at run time. Whole captured images are still used for VU0,
# and for VU1 code that did not come from the executable (vu-captured/).
python3 "$ROOT/ps2recomp/diagnostics/extract_vu_programs.py" "$ROOT/orig/SLUS_213.76" "$D/vu-programs" >/dev/null
IMAGES=("$D"/vu-programs/prog[01]-*.bin "$D"/vu-coverage-level00/vu0-*.bin "$D"/vu-trace/images/*/vu0-*.bin "$D"/vu-captured/vu0-*.bin)
(( ${#IMAGES[@]} )) || { echo "nenhuma imagem VU capturada" >&2; exit 1; }
"$BIN/ps2x_vu1_recompile" "$OUT" "${ARGS[@]}" "${IMAGES[@]}"
# The core includes the generated file only when it exists (__has_include), so
# the build system cannot see the dependency the first time: force a recompile.
touch "$ROOT/tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp"

"$NINJA" -C "$ROOT/tools/PS2Recomp/out/rt" -j8 ps2x_vu1_trace_replay
for TRACE in "${TRACES[@]}"; do
  echo "== $(basename "$TRACE")"
  "$BIN/ps2x_vu1_trace_replay" "$TRACE" --engine recompiled --bench 2 | tail -3
done
"$NINJA" -C "$ROOT/tools/PS2Recomp/out/rt" -j8 ps2EntryRunner
