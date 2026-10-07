# Handoff: corrupção de texturas no Level_00 — gating do PATH3 por MSKPATH3 (2026-10-07)

Ler junto: `ps2recomp/PS2_PROJECT_STATE.md`, `ps2recomp/GPU_BACKEND_PLAN.md`,
`ps2recomp/diagnostics/CLUT_OVERLAP_LEVEL00.md`, `.kiro/skills/ps2-recomp/SKILL.md`.

## Evidência nova (comparação com PCSX2)

Ferramentas novas em `ps2recomp/diagnostics/`:
- `audit_texture_sequence.py` = `audit_texture_uploads.py` + grava `<extract>.seq`
  (uma linha por upload completo `U ev path bp psm w h bw x y` e por TEX0 `T ev path tbp cbp psm cld`).
- `clut_last_writer.py <arquivo.seq>`: para cada TEX0 indexado com CLD≠0, quem escreveu por último o bloco da CBP.

Uso:
```
python3 ps2recomp/diagnostics/audit_texture_sequence.py CAPTURA.bin --all-transfers --extract /tmp/x.bin
python3 ps2recomp/diagnostics/clut_last_writer.py /tmp/x.bin.seq
```

Resultados:

| | PCSX2 (`recomp/diagnostics/pcsx2-level00-events.bin`) | Recomp (`recomp/diagnostics/level00-sdl-current-gif.bin`) |
|---|---|---|
| CLUT lida vem de upload de paleta (CT32 ≤256 texels) | 4692/4692 (100%) | 12.571 |
| CLUT já sobrescrita por textura maior | 0 | 52.125 (~80%) |
| Máx. uploads sem TEX0 entre eles | 151 | 1.578 |
| Máx. uploads num evento GIF | 6 | 1.578 |

- Conteúdo dos uploads bate com o PCSX2 (mesmos SHA dos assets); só a **ordem** está errada.
- PCSX2: 2 frames de jogo no dump, 700 uploads/frame, sempre "sobe textura+paleta → PATH1 desenha → próximo lote reusa slot".
- Recomp: a cada frame um único evento PATH3 de 4.792.368 bytes (4025 GIFtags, **14 EOPs**, 1578 IMAGE)
  repete idêntico; enche o pool (BP 11616…~15000) e volta a sobrescrever slots antes de qualquer draw.
- A captura `PS2X_GIF_RAW_CAPTURE` é feita no limite do renderer (`ps2_gif_arbiter.cpp`), logo reflete a ordem real que o GS recebe.

## Simulação offline (sem rodar o jogo)

Liberando a cadeia de 4,79 MB pacote a pacote (por EOP) e reavaliando os 4.246 draws indexados do frame seguinte:
- comportamento atual (cadeia inteira antes do PATH1): **765/4.246 (18%)** com paleta válida;
- parada em cada EOP com avanço monotônico: **4.232/4.246 (99,7%)**, só 14 falhas.

## Hipótese de causa

GIF_STAT capturado antes = `0x06` (M3P + IMT): o Black controla PATH3 por `MSKPATH3` no stream VIF1.
No runtime, quando a cadeia GIF chega com PATH3 mascarado ela entra inteira em `m_path3MaskedFifo`
como um único pacote, e o primeiro `MSKPATH3(0)` (`ps2_vif1_interpreter.cpp`) chama
`flushMaskedPath3Packets()`, que despeja **a fila toda**. No hardware a máscara vale no fim do pacote
PATH3 corrente, então cada unmask deveria liberar só até o próximo EOP (a confirmar no `Gif_Unit` do PCSX2).
Por isso o protótipo `PS2X_GIF_INCREMENTAL` (fatiar por EOP sem amarrar ao MSKPATH3) não mudou a imagem.

## Patch já aplicado (opt-in, sem efeito sem a variável)

Arquivos: `tools/PS2Recomp/ps2xRuntime/src/lib/ps2_memory.cpp` e `ps2_vif1_interpreter.cpp`
(ainda **não** regenerado em `ps2recomp/patches/0001-black-runtime-fixes.patch`).
- `PS2X_PATH3_EOP_GATE=1`: em `submitGifPacket`, com PATH3 mascarado, a cadeia é dividida por EOP
  (`gifEopPacketSize`) ao entrar na fila mascarada.
- No `VIF_MSKPATH3` 1→0, `g_ps2xPath3ReleaseOnePacket=true` faz `flushMaskedPath3Packets` liberar só o
  primeiro pacote. Os demais chamadores (reset VIF1, novo PATH3 sem máscara) continuam despejando tudo
  para preservar a ordem.
- Estatísticas `[path3-gate]` no stderr (também com `PS2X_PATH3_GATE_STATS=1` sem o gate):
  cadeias mascaradas/não mascaradas no kick, unmasks, pacotes enfileirados/liberados, fifo máximo.
- Passou em `g++ -fsyntax-only`; **ainda não compilado no Mac nem testado no jogo**.

Atenção: o `PS2X_GIF_INCREMENTAL` está ligado por padrão no código atual (`incrementalGifDmaEnabled`),
com o pacing `onPath1Progress` (crédito 0,52). Com PATH3 mascarado no kick, o caminho cai no `else`
que submete a cadeia inteira → fila mascarada → o gate atua. Se as estatísticas mostrarem cadeias
**não mascaradas** no kick, o gate não atua e o próximo passo é segurar a cadeia até o primeiro
MSKPATH3 do frame.

## Teste pronto

`bash ps2recomp/diagnostics/TestarPath3Gate.command` (chama `path3_gate_test.py`):
1. `ninja -C tools/PS2Recomp/out/rt ps2EntryRunner` → `recomp/diagnostics/path3-gate/build.log`
2. roda `baseline` e depois `gate` com o mesmo roteiro de pad (o de `run_sdl_gpu_capture.py`),
   SDL_GPU, BLACK_DEBUG=1, captura GIF armada ao chegar no Level_00, ~75 s na fase;
3. roda o verificador de CLUT em cada captura, guarda `[path3-gate]`, as últimas linhas `upd=` e
   frames `*-frameN.ppm` (de `/tmp/black_frame_*.ppm`);
4. tudo resumido em `recomp/diagnostics/path3-gate/STATUS.txt`.

Rodar só um modo: `bash ... TestarPath3Gate.command gate`.

## Critérios de sucesso

- `gate`: `overwritten_by_other` ≈ 0 e `palette_upload` ≈ 100%, como no PCSX2;
- frame do Level_00 com paredes/materiais corretos (comparar com `recomp/diagnostics/pcsx2-live-reference-20261006.png`);
- sem freeze e sem `max-fifo` crescendo sem parar (crescimento = unmasks insuficientes → modelar FLUSHA/espera do guest);
- updates/s não piores que o baseline (~2–3/s no Level_00).

Se passar: tornar o padrão, regenerar o patch 0001 (comando no SKILL.md §0), atualizar
`PS2_PROJECT_STATE.md` e `CLUT_OVERLAP_LEVEL00.md`.

## Resultado — 2026-10-07 (fechado)

Passou em todos os critérios e virou padrão; `PS2X_PATH3_EOP_GATE=0` desliga. O teste agora aceita os modos
`default` (variável ausente), `gate` (=1) e `baseline` (=0), salva `<modo>-frameN.ppm`, repassa `EXTRA_<VAR>=x`
ao runner e não zera o STATUS com `STATUS_APPEND=1`. O report `[path3-gate]` sai a cada 256 unmasks com o
tamanho real da fila (antes imprimia sempre `fifo=0`). Patch 0001 regenerado. Números em
`ps2recomp/PS2_PROJECT_STATE.md`, seção de 2026-10-07. Pendente: `PS2Recomp-local-changes.patch` (README) e
desempenho na fase (~0,8 upd/s nos dois modos).
