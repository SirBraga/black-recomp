#!/usr/bin/env bash
# Roda o Black recompilado pelo PS2Recomp por N segundos e salva o log.
# Uso: ps2recomp/run.sh [segundos=30] [log=/tmp/black_run.log]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$ROOT/tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner"
DISC="$ROOT/recomp/disc"
SECS="${1:-30}"
LOG="${2:-/tmp/black_run.log}"

[[ -x "$RUNNER" ]] || { echo "runner nao encontrado: $RUNNER (rode ps2recomp/build.sh)" >&2; exit 1; }
[[ -f "$DISC/SLUS_213.76" ]] || { echo "disco nao extraido em $DISC" >&2; exit 1; }

if [[ -z "${BLACK_CD_IMAGE+x}" && -f "$ROOT/orig/Black.iso" ]]; then
    export BLACK_CD_IMAGE="$ROOT/orig/Black.iso"   # BLACK_CD_IMAGE= (vazio) forca a ISO virtual
fi

export BLACK_DEBUG="${BLACK_DEBUG-1}"   # run.sh e o runner de diagnostico; jogar direto roda sem hooks
export BLACK_CUTSCENE_SKIP="${BLACK_CUTSCENE_SKIP:-1}"   # Tab / Select pede Video::stop; BLACK_CUTSCENE_SKIP=0 desliga
cd "$DISC"
"$RUNNER" "$DISC/SLUS_213.76" >"$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 "$SECS"); do
    sleep 1
    kill -0 "$PID" 2>/dev/null || break
done
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null || true
    sleep 1
    kill -9 "$PID" 2>/dev/null || true
    echo "[run.sh] encerrado apos ${SECS}s" >>"$LOG"
else
    wait "$PID" || echo "[run.sh] processo saiu com codigo $?" >>"$LOG"
fi
echo "log: $LOG ($(wc -l <"$LOG") linhas)"
