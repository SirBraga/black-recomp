#!/usr/bin/env bash
# Build the optional GPU module separately from the large recompiled runner.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SOURCE="${BLACK_PARALLEL_SOURCE:-$ROOT/recomp/gpu/parallel-gs}"
OUT="$ROOT/recomp/gpu/build"
if [[ ! -d "$SOURCE/.git" ]]; then
 mkdir -p "$(dirname "$SOURCE")"
 git clone https://github.com/Arntzen-Software/parallel-gs "$SOURCE"
 git -C "$SOURCE" checkout 3a66c1976170cbc2cb53a3593fabbc7c4b2ccfbd
fi
[[ "$(git -C "$SOURCE" rev-parse HEAD)" == 3a66c1976170cbc2cb53a3593fabbc7c4b2ccfbd ]] || { echo 'Unexpected paraLLEl-GS version' >&2; exit 1; }
git -C "$SOURCE" submodule update --init --depth 1 Granite
git -C "$SOURCE/Granite" submodule update --init --depth 1 third_party/volk third_party/khronos/vulkan-headers third_party/rapidjson
PATCH="$ROOT/ps2recomp/dependencies/granite-macos-timer.diff"
if git -C "$SOURCE/Granite" apply --check "$PATCH" 2>/dev/null; then
 git -C "$SOURCE/Granite" apply "$PATCH"
elif ! git -C "$SOURCE/Granite" apply --reverse --check "$PATCH" 2>/dev/null; then
 echo 'Granite macOS patch does not apply' >&2; exit 1
fi
PAGE_PATCH="$ROOT/ps2recomp/dependencies/parallel-gs-page-rect.diff"
if git -C "$SOURCE" apply --check "$PAGE_PATCH" 2>/dev/null; then
 git -C "$SOURCE" apply "$PAGE_PATCH"
elif ! git -C "$SOURCE" apply --reverse --check "$PAGE_PATCH" 2>/dev/null; then
 echo 'paraLLEl-GS page rectangle patch does not apply' >&2; exit 1
fi
GIF_PATCH="$ROOT/ps2recomp/dependencies/parallel-gs-gif-bounds.diff"
if git -C "$SOURCE" apply --check "$GIF_PATCH" 2>/dev/null; then
 git -C "$SOURCE" apply "$GIF_PATCH"
elif ! git -C "$SOURCE" apply --reverse --check "$GIF_PATCH" 2>/dev/null; then
 echo 'paraLLEl-GS GIF bounds patch does not apply' >&2; exit 1
fi
SCANOUT_PATCH="$ROOT/ps2recomp/dependencies/parallel-gs-internal-scanout.diff"
if git -C "$SOURCE" apply --check "$SCANOUT_PATCH" 2>/dev/null; then
 git -C "$SOURCE" apply "$SCANOUT_PATCH"
elif ! git -C "$SOURCE" apply --reverse --check "$SCANOUT_PATCH" 2>/dev/null; then
 echo 'paraLLEl-GS internal scanout patch does not apply' >&2; exit 1
fi
CLUT_PATCH="$ROOT/ps2recomp/dependencies/parallel-gs-clut-snapshot.diff"
if git -C "$SOURCE" apply --check "$CLUT_PATCH" 2>/dev/null; then
 git -C "$SOURCE" apply "$CLUT_PATCH"
elif ! git -C "$SOURCE" apply --reverse --check "$CLUT_PATCH" 2>/dev/null; then
 echo 'paraLLEl-GS CLUT diagnostic patch does not apply' >&2; exit 1
fi
METAL_EXPORT_PATCH="$ROOT/ps2recomp/dependencies/parallel-gs-metal-export.diff"
if git -C "$SOURCE" apply --check "$METAL_EXPORT_PATCH" 2>/dev/null; then
 git -C "$SOURCE" apply "$METAL_EXPORT_PATCH"
elif ! git -C "$SOURCE" apply --reverse --check "$METAL_EXPORT_PATCH" 2>/dev/null; then
 echo 'paraLLEl-GS Metal export patch does not apply' >&2; exit 1
fi
cmake -S "$ROOT/ps2recomp/gpu" -B "$OUT" -G Ninja \
 -DCMAKE_MAKE_PROGRAM="$ROOT/.venv/bin/ninja" -DCMAKE_BUILD_TYPE=Release \
 -DGS_SOURCE="$SOURCE" -DRUNTIME_INCLUDE="$ROOT/tools/PS2Recomp/ps2xRuntime/include"
"$ROOT/.venv/bin/ninja" -C "$OUT" -j6 black-parallel-gs black-parallel-module-test black-parallel-texture-test gs-replay black-native-gs
printf 'GPU module: %s/libblack-parallel-gs.so\n' "$OUT"
