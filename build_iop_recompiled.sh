#!/usr/bin/env bash
# Regenerates the statically recompiled IOP module code
# (tools/PS2Recomp/ps2xIOP/src/emulator/black_iop_recompiled.inc) and rebuilds the runner.
#
# The module text comes from recomp/diagnostics/iop-captured/: the runner writes it there (already
# relocated to its load address) when started with PS2X_IOP_RECOMP_CAPTURE=<that directory>; every IRX
# the game loads at boot is captured in the first seconds. Local files only: they contain game code.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/recomp/diagnostics/iop-captured"
OUT="$ROOT/tools/PS2Recomp/ps2xIOP/src/emulator/black_iop_recompiled.inc"
shopt -s nullglob
MODULES=("$D"/iop-*.bin)
(( ${#MODULES[@]} )) || { echo "nenhum modulo IOP capturado em $D (rode o jogo com PS2X_IOP_RECOMP_CAPTURE=$D)" >&2; exit 1; }
python3 "$ROOT/ps2recomp/diagnostics/iop_recompile.py" "$OUT" "${MODULES[@]}"
# The emulator includes the generated file only when it exists (__has_include): force a recompile.
touch "$ROOT/tools/PS2Recomp/ps2xIOP/src/emulator/iop_emulator.cpp"
"$ROOT/.venv/bin/ninja" -C "$ROOT/tools/PS2Recomp/out/rt" -j8 ps2EntryRunner
