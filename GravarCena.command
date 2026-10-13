#!/bin/bash
# Abre o jogo com o paraLLEl-GS e a gravação do GS armada: aperte F9 no lugar que quer gravar. Os próximos 120
# quadros vão para recomp/diagnostics/gs-dump/cena.gsdump (para comparar o renderizador nativo com a referência:
# gs_compare.sh cena). O resto das opções é o do JogarBlack.command.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/recomp/diagnostics/gs-dump"
export BLACK_GS=parallel
export PS2X_GS_DUMP="$ROOT/recomp/diagnostics/gs-dump/cena.gsdump"
export PS2X_GS_DUMP_REQUEST="$ROOT/recomp/diagnostics/gs-dump/cena.request"
rm -f "$PS2X_GS_DUMP_REQUEST"
exec "$ROOT/ps2recomp/JogarBlack.command"
