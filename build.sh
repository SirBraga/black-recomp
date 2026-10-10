#!/usr/bin/env bash
# Pipeline completo PS2Recomp para Black (SLUS_213.76):
#   analyzer -> entry_points por ponteiro -> recompilador -> copia para o runner -> build nativo
# Uso: ps2recomp/build.sh [--skip-recomp]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
P2R="$ROOT/tools/PS2Recomp"
NINJA="$ROOT/.venv/bin/ninja"
ELF="$ROOT/orig/SLUS_213.76"
OUT="$ROOT/recomp/output"
CFG="$ROOT/recomp/black.toml"
RUNNER_SRC="$P2R/ps2xRuntime/src/runner"
RUNNER_INC="$P2R/ps2xRuntime/include"

[[ -f "$ELF" ]] || { echo "ELF nao encontrado: $ELF" >&2; exit 1; }
[[ -d "$P2R" ]] || { echo "clone o PS2Recomp em $P2R (ver ps2recomp/README.md)" >&2; exit 1; }

# Correcoes nossas no runtime/IOP (ver ps2recomp/patches/). Idempotente.
for p in "$ROOT"/ps2recomp/patches/*.patch; do
    if git -C "$P2R" apply --check "$p" 2>/dev/null; then
        git -C "$P2R" apply "$p" && echo "patch aplicado: $(basename "$p")"
    elif ! git -C "$P2R" apply --check -R "$p" 2>/dev/null; then
        # arvore ja tem o patch + edicoes locais por cima (ou upstream mudou): segue, mas avisa
        echo "aviso: $(basename "$p") nao aplica limpo; assumindo edicoes locais em $P2R" >&2
    fi
done

if [[ ! -x "$P2R/out/build/ps2xRecomp/ps2_recomp" ]]; then
    cmake -S "$P2R" -B "$P2R/out/build" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE=Release \
        -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_RUNTIME=OFF
    "$NINJA" -C "$P2R/out/build"
fi

if [[ "${1:-}" != "--skip-recomp" ]]; then
    # Rebuild the translator after applying fixes before regenerating guest code.
    "$NINJA" -C "$P2R/out/build" ps2_recomp
    mkdir -p "$ROOT/recomp"
    cd "$ROOT"
    "$P2R/out/build/ps2xAnalyzer/ps2_analyzer" "$ELF" "$CFG" >"$ROOT/recomp/analyzer.log" 2>&1
    # caminhos relativos a raiz do repo
    sed -i '' -e "s#^input = .*#input = \"orig/SLUS_213.76\"#" -e "s#^output = .*#output = \"recomp/output/\"#" "$CFG"
    python3 "$ROOT/ps2recomp/find_code_pointers.py" "$ELF" --toml "$CFG"
    python3 "$ROOT/ps2recomp/add_instruction_patches.py" "$CFG"
    rm -rf "$OUT"
    "$P2R/out/build/ps2xRecomp/ps2_recomp" "$CFG" >"$ROOT/recomp/recomp.log" 2>&1
    grep -A12 "PS2Recomp report" "$ROOT/recomp/recomp.log" || true

    # troca a saida gerada no runner (remove a anterior para nao sobrar funcao velha)
    find "$RUNNER_SRC" -maxdepth 1 \( -name 'sub_*.cpp' -o -name 'entry_*.cpp' -o -name 'register_functions.cpp' \) -delete
    cp "$OUT"/*.cpp "$RUNNER_SRC"/
    cp "$OUT"/*.h "$RUNNER_INC"/
fi

cp "$ROOT"/ps2recomp/overrides/*.cpp "$RUNNER_SRC"/
for f in "$ROOT"/ps2recomp/overrides/*.cpp; do mv "$RUNNER_SRC/$(basename "$f")" "$RUNNER_SRC/zz_$(basename "$f")"; done

if [[ ! -f "$P2R/out/rt/build.ninja" ]]; then
    cmake -S "$P2R" -B "$P2R/out/rt" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE=Release \
        -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF \
        -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=64
fi
RAYLIB_SRC="$P2R/out/rt/_deps/raylib-src"
RAYLIB_PATCH="$ROOT/ps2recomp/dependencies/raylib-window-init.diff"
if patch --batch --forward --dry-run -d "$RAYLIB_SRC" -p1 < "$RAYLIB_PATCH" >/dev/null 2>&1; then
    patch --batch --forward -d "$RAYLIB_SRC" -p1 < "$RAYLIB_PATCH"
elif ! patch --batch --dry-run -R -d "$RAYLIB_SRC" -p1 < "$RAYLIB_PATCH" >/dev/null 2>&1; then
    echo "erro: correcao de inicializacao Raylib nao aplica em $RAYLIB_SRC" >&2
    exit 1
fi
"$NINJA" -C "$P2R/out/rt" -j8 ps2EntryRunner
echo "ok: $P2R/out/rt/ps2xRuntime/ps2EntryRunner"
