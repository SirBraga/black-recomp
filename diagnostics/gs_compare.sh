#!/usr/bin/env bash
# Replays every GS dump in recomp/diagnostics/gs-dump through paraLLEl-GS (once, kept) and through the native
# renderer, then prints how far the native frames are from the reference.   gs_compare.sh [dump names...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$ROOT/recomp/diagnostics/gs-dump"; B="$ROOT/recomp/gpu/build"
EVERY="${EVERY:-5}"; TOLERANCE="${TOLERANCE:-24}"
names=("$@"); if [[ ${#names[@]} -eq 0 ]]; then for f in "$D"/*.gsdump; do names+=("$(basename "$f" .gsdump)"); done; fi
for name in "${names[@]}"; do
 if [[ ! -d "$D/$name-parallel" || "$D/$name.gsdump" -nt "$D/$name-parallel" ]]; then
  rm -rf "$D/$name-parallel"
  "$B/gs-replay" "$D/$name.gsdump" --module "$B/libblack-parallel-gs.so" --moltenvk "$ROOT/recomp/gpu/moltenvk/libMoltenVK.dylib" --out "$D/$name-parallel" --every "$EVERY" >/dev/null 2>&1
 fi
 rm -rf "$D/$name-native" "$D/$name-diff"
 timing="$("$B/gs-replay" "$D/$name.gsdump" --module "$B/libblack-native-gs.so" --out "$D/$name-native" --every "$EVERY" 2>&1 | grep 'ms per scanout' || echo 'replay failed')"
 "$B/gs-replay" --diff "$D/$name-parallel" "$D/$name-native" --out "$D/$name-diff" --tolerance "$TOLERANCE" > "$D/$name-diff.txt" || true
 printf '%-8s %s | %s | worst frame: %s\n' "$name" "$timing" "$(tail -1 "$D/$name-diff.txt")" "$(grep '^frame' "$D/$name-diff.txt" | sort -t: -k2 -rn | head -1 | cut -c1-60)"
done
