#!/usr/bin/env bash
# Build opt-in VU instruction bodies from a local captured image (not distributed).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${1:?uso: build_vu_experimental.sh imagem-vu1.bin coverage.bin [segunda-vu1.bin] [imagens-vu0.bin ...] [--vu1 imagem-vu1.bin ...]}"
COVERAGE="${2:?informe coverage.bin [segunda-imagem.bin] [imagens-vu0.bin ...]}"
HEADER="$ROOT/tools/PS2Recomp/ps2xRuntime/src/lib/vu/black_vu_aot_generated.hpp"
BUNDLE_ARGS=("$IMAGE" "$COVERAGE" "$HEADER" "${3:-}")
EXTRA_ARGS=("${@:4}")
for ((INDEX=0; INDEX<${#EXTRA_ARGS[@]}; INDEX++)); do
  case "${EXTRA_ARGS[$INDEX]}" in
    --vu0|--vu1)
      FLAG="${EXTRA_ARGS[$INDEX]}"
      ((INDEX+=1))
      if ((INDEX >= ${#EXTRA_ARGS[@]})); then echo "$FLAG exige um arquivo .bin" >&2; exit 2; fi
      BUNDLE_ARGS+=("$FLAG" "${EXTRA_ARGS[$INDEX]}")
      ;;
    --vu0=*|--vu1=*)
      FLAG="${EXTRA_ARGS[$INDEX]%%=*}"
      IMAGE_ARG="${EXTRA_ARGS[$INDEX]#*=}"
      BUNDLE_ARGS+=("$FLAG" "$IMAGE_ARG")
      ;;
    *) BUNDLE_ARGS+=(--vu0 "${EXTRA_ARGS[$INDEX]}") ;;
  esac
done
python3 "$ROOT/ps2recomp/diagnostics/build_vu_aot_bundle.py" "${BUNDLE_ARGS[@]}"
"$ROOT/.venv/bin/ninja" -C "$ROOT/tools/PS2Recomp/out/rt" -j8 ps2EntryRunner
